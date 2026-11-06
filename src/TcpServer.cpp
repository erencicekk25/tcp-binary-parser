#include "TcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace tbp {

static_assert(std::atomic<bool>::is_always_lock_free,
              "TcpServer::stop() must be async-signal-safe");

namespace {

constexpr std::size_t kReceiveChunkSize = 512;

// The buffer never needs to hold more than one maximum-size frame plus one
// receive chunk; anything beyond that indicates a logic error.
constexpr std::size_t kMaxClientBufferSize =
    protocol::kHeaderSize + protocol::kMaxPayloadSize + kReceiveChunkSize;

std::system_error makeSystemError(const char* what) {
    return std::system_error(errno, std::generic_category(), what);
}

std::string errorText(int error_code) { return std::generic_category().message(error_code); }

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw makeSystemError("fcntl(F_GETFL)");
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw makeSystemError("fcntl(F_SETFL)");
    }
}

std::string formatPeer(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &address.sin_addr, text.data(), text.size()) == nullptr) {
        return "<unknown>";
    }
    return std::string(text.data()) + ":" + std::to_string(ntohs(address.sin_port));
}

void logInfo(const ClientInfo& client, const std::string& message) {
    std::clog << "[client " << client.id << " " << client.address << "] " << message << '\n';
}

void logWarning(const ClientInfo& client, const std::string& message) {
    std::cerr << "[client " << client.id << " " << client.address << "] warning: " << message
              << '\n';
}

}  // namespace

// ---------------------------------------------------------------------------
// FileDescriptor
// ---------------------------------------------------------------------------

FileDescriptor::~FileDescriptor() { reset(); }

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.fd_, -1));
    }
    return *this;
}

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0) {
        // EINTR on close() must not be retried on Linux; the descriptor is
        // released either way.
        ::close(fd_);
    }
    fd_ = fd;
}

// ---------------------------------------------------------------------------
// TcpServer
// ---------------------------------------------------------------------------

TcpServer::TcpServer(ServerConfig config) : config_(std::move(config)) {
    if (config_.backlog <= 0) {
        throw std::invalid_argument("backlog must be positive");
    }
    if (config_.max_clients == 0) {
        throw std::invalid_argument("max_clients must be positive");
    }
    if (config_.poll_timeout_ms <= 0) {
        throw std::invalid_argument("poll_timeout_ms must be positive");
    }
}

void TcpServer::setSensorHandler(SensorHandler handler) { sensor_handler_ = std::move(handler); }

void TcpServer::start() {
    if (listen_socket_.valid()) {
        throw std::logic_error("TcpServer::start() called twice");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 bind address: " + config_.bind_address);
    }

    FileDescriptor socket_fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket_fd.valid()) {
        throw makeSystemError("socket");
    }

    const int enable = 1;
    if (::setsockopt(socket_fd.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        throw makeSystemError("setsockopt(SO_REUSEADDR)");
    }

    if (::bind(socket_fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw makeSystemError("bind");
    }
    if (::listen(socket_fd.get(), config_.backlog) < 0) {
        throw makeSystemError("listen");
    }
    setNonBlocking(socket_fd.get());

    // Resolve the actual port when an ephemeral port (0) was requested.
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(socket_fd.get(), reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        config_.port = ntohs(bound.sin_port);
    }

    listen_socket_ = std::move(socket_fd);
    std::clog << "Listening on " << config_.bind_address << ":" << config_.port << '\n';
}

void TcpServer::stop() noexcept { running_.store(false); }

void TcpServer::run() {
    if (!listen_socket_.valid()) {
        throw std::logic_error("TcpServer::run() called before start()");
    }

    running_.store(true);
    std::vector<pollfd> poll_fds;

    while (running_.load()) {
        poll_fds.clear();
        poll_fds.push_back({listen_socket_.get(), POLLIN, 0});
        for (const Client& client : clients_) {
            poll_fds.push_back({client.socket.get(), POLLIN, 0});
        }

        const int ready = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()),
                                 config_.poll_timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by a signal; re-check running_.
            }
            throw makeSystemError("poll");
        }
        if (ready == 0) {
            continue;
        }

        // Client entries map 1:1 to poll_fds[1..]. New connections are only
        // appended after this loop, so the indices stay valid here.
        for (std::size_t i = 0; i < clients_.size(); ++i) {
            Client& client = clients_[i];
            const short events = poll_fds[i + 1].revents;
            bool keep_open = true;

            if ((events & POLLIN) != 0) {
                keep_open = readFromClient(client);
            } else if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                logWarning(client.info, "socket error or hang-up");
                keep_open = false;
            }

            if (!keep_open) {
                logInfo(client.info, "disconnected");
                client.socket.reset();
            }
        }

        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [](const Client& c) { return !c.socket.valid(); }),
                       clients_.end());

        if ((poll_fds[0].revents & POLLIN) != 0) {
            acceptConnections();
        } else if ((poll_fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
            throw std::runtime_error("listening socket reported an error");
        }
    }

    std::clog << "Shutting down, closing " << clients_.size() << " client connection(s)\n";
    clients_.clear();
    listen_socket_.reset();
}

void TcpServer::acceptConnections() {
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        FileDescriptor client_fd(
            ::accept(listen_socket_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_len));

        if (!client_fd.valid()) {
            const int error_code = errno;
            if (error_code == EAGAIN || error_code == EWOULDBLOCK) {
                return;  // No more pending connections.
            }
            if (error_code == EINTR || error_code == ECONNABORTED || error_code == EPROTO) {
                continue;  // Transient; try the next pending connection.
            }
            // Resource exhaustion (EMFILE, ENFILE, ENOBUFS, ...) should not
            // take the whole server down; retry on the next poll cycle.
            std::cerr << "accept failed: " << errorText(error_code) << '\n';
            return;
        }

        ClientInfo info{next_client_id_++, formatPeer(peer)};

        if (clients_.size() >= config_.max_clients) {
            logWarning(info, "rejected, client limit reached");
            continue;  // client_fd is closed by its destructor.
        }

        try {
            setNonBlocking(client_fd.get());
        } catch (const std::system_error& error) {
            logWarning(info, std::string("rejected: ") + error.what());
            continue;
        }

        logInfo(info, "connected");
        Client client{std::move(client_fd), std::move(info), {}};
        client.buffer.reserve(kMaxClientBufferSize);
        clients_.push_back(std::move(client));
    }
}

bool TcpServer::readFromClient(Client& client) {
    std::array<std::uint8_t, kReceiveChunkSize> chunk{};

    while (true) {
        const ssize_t received = ::recv(client.socket.get(), chunk.data(), chunk.size(), 0);

        if (received > 0) {
            const auto count = static_cast<std::size_t>(received);
            if (client.buffer.size() + count > kMaxClientBufferSize) {
                logWarning(client.info, "receive buffer overflow, dropping connection");
                return false;
            }
            client.buffer.insert(client.buffer.end(), chunk.begin(),
                                 chunk.begin() + static_cast<std::ptrdiff_t>(count));
            processBuffer(client);
            continue;
        }

        if (received == 0) {
            if (!client.buffer.empty()) {
                logWarning(client.info, std::to_string(client.buffer.size()) +
                                            " byte(s) of incomplete packet discarded");
            }
            return false;  // Orderly shutdown by the peer.
        }

        const int error_code = errno;
        if (error_code == EAGAIN || error_code == EWOULDBLOCK) {
            return true;  // Drained everything available for now.
        }
        if (error_code == EINTR) {
            continue;
        }

        logWarning(client.info, "recv failed: " + errorText(error_code));
        return false;
    }
}

void TcpServer::processBuffer(Client& client) {
    std::vector<std::uint8_t>& buffer = client.buffer;
    std::size_t offset = 0;

    while (offset < buffer.size()) {
        if (buffer[offset] != protocol::kSyncByte) {
            const auto first = buffer.begin() + static_cast<std::ptrdiff_t>(offset);
            const auto sync = std::find(first, buffer.end(), protocol::kSyncByte);
            const auto skipped = static_cast<std::size_t>(std::distance(first, sync));
            logWarning(client.info,
                       "skipped " + std::to_string(skipped) + " byte(s) while searching for sync");
            offset += skipped;
            continue;
        }

        const std::uint8_t* frame = buffer.data() + offset;
        const std::size_t available = buffer.size() - offset;

        ParseError error = ParseError::None;
        const std::optional<std::size_t> frame_size = PacketParser::frameSize(frame, available, &error);

        if (!frame_size) {
            if (error == ParseError::BufferTooSmall) {
                break;  // Header not complete yet; wait for more data.
            }
            // A sync byte followed by a nonsensical header: treat it as noise
            // and resynchronize from the next byte.
            logWarning(client.info, std::string("invalid header: ") + toString(error));
            ++offset;
            continue;
        }

        if (available < *frame_size) {
            break;  // Payload not complete yet.
        }

        dispatchFrame(client, frame, *frame_size);
        offset += *frame_size;
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
}

void TcpServer::dispatchFrame(const Client& client, const std::uint8_t* frame, std::size_t size) {
    const auto command = static_cast<CommandId>(frame[1]);

    switch (command) {
        case CommandId::SensorData: {
            ParseError error = ParseError::None;
            const std::optional<SensorPayload> payload =
                PacketParser::parseSensorPacket(frame, size, &error);
            if (!payload) {
                logWarning(client.info, std::string("sensor packet rejected: ") + toString(error));
                return;
            }
            if (!sensor_handler_) {
                return;
            }
            try {
                sensor_handler_(client.info, *payload);
            } catch (const std::exception& ex) {
                logWarning(client.info, std::string("sensor handler threw: ") + ex.what());
            } catch (...) {
                logWarning(client.info, "sensor handler threw an unknown exception");
            }
            return;
        }
    }

    logWarning(client.info, "unknown command id " + std::to_string(frame[1]) + ", frame skipped");
}

}  // namespace tbp
