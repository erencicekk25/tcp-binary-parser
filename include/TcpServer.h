#ifndef TCP_BINARY_PARSER_TCP_SERVER_H
#define TCP_BINARY_PARSER_TCP_SERVER_H

#if defined(_WIN32)
#error "TcpServer relies on POSIX Berkeley sockets and is not available on Windows. Use WSL or a POSIX target."
#endif

#include "PacketProtocol.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tbp {

// Owns a POSIX file descriptor and closes it on destruction.
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 9000;
    int backlog = 16;
    std::size_t max_clients = 32;
    int poll_timeout_ms = 500;
};

struct ClientInfo {
    int id = 0;
    std::string address;
};

// Single-threaded, poll()-based TCP server. Incoming bytes are treated as a
// stream: packets may arrive fragmented or coalesced, and the server
// re-synchronizes on the sync byte after corrupted data.
class TcpServer {
public:
    using SensorHandler = std::function<void(const ClientInfo&, const SensorPayload&)>;

    explicit TcpServer(ServerConfig config);
    ~TcpServer() = default;

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    void setSensorHandler(SensorHandler handler);

    // Creates, binds and listens on the server socket.
    // Throws std::system_error or std::invalid_argument on failure.
    void start();

    // Runs the event loop until stop() is called. Throws std::logic_error if
    // start() has not been called and std::system_error on fatal poll errors.
    void run();

    // Requests the event loop to exit. Safe to call from a signal handler
    // when std::atomic<bool> is lock-free (checked at compile time).
    void stop() noexcept;

    std::uint16_t port() const noexcept { return config_.port; }

private:
    struct Client {
        FileDescriptor socket;
        ClientInfo info;
        std::vector<std::uint8_t> buffer;
    };

    void acceptConnections();
    bool readFromClient(Client& client);
    void processBuffer(Client& client);
    void dispatchFrame(const Client& client, const std::uint8_t* frame, std::size_t size);

    ServerConfig config_;
    FileDescriptor listen_socket_;
    std::vector<Client> clients_;
    SensorHandler sensor_handler_;
    std::atomic<bool> running_{false};
    int next_client_id_ = 1;
};

}  // namespace tbp

#endif  // TCP_BINARY_PARSER_TCP_SERVER_H
