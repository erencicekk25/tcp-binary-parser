#include "PacketProtocol.hpp"
#include "TcpServer.h"

#include <signal.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

std::atomic<tbp::TcpServer*> g_active_server{nullptr};
static_assert(std::atomic<tbp::TcpServer*>::is_always_lock_free,
              "signal handler requires a lock-free atomic pointer");

void handleShutdownSignal(int /*signal_number*/) {
    if (tbp::TcpServer* server = g_active_server.load()) {
        server->stop();
    }
}

void installSignalHandlers() {
    struct sigaction action {};
    action.sa_handler = handleShutdownSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // No SA_RESTART: let poll() return EINTR promptly.

    if (::sigaction(SIGINT, &action, nullptr) < 0 || ::sigaction(SIGTERM, &action, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "sigaction");
    }

    // A peer closing its socket must never terminate the process.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (::sigaction(SIGPIPE, &ignore, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "sigaction(SIGPIPE)");
    }
}

struct Options {
    tbp::ServerConfig server;
    bool self_test = false;
    bool show_help = false;
};

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  -p, --port <port>      TCP port to listen on (default: 9000, 0 = any)\n"
              << "  -b, --bind <address>   IPv4 address to bind to (default: 0.0.0.0)\n"
              << "  -c, --max-clients <n>  Maximum simultaneous clients (default: 32)\n"
              << "      --self-test        Encode and decode a sample packet, then exit\n"
              << "  -h, --help             Show this help message\n";
}

template <typename T>
std::optional<T> parseNumber(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    T value{};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Returns std::nullopt and prints a diagnostic when the arguments are invalid.
std::optional<Options> parseArguments(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        const auto requireValue = [&](std::string_view name) -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
        } else if (arg == "--self-test") {
            options.self_test = true;
        } else if (arg == "-p" || arg == "--port") {
            const auto value = requireValue(arg);
            if (!value) {
                return std::nullopt;
            }
            const auto port = parseNumber<std::uint16_t>(*value);
            if (!port) {
                std::cerr << "error: invalid port '" << *value << "'\n";
                return std::nullopt;
            }
            options.server.port = *port;
        } else if (arg == "-b" || arg == "--bind") {
            const auto value = requireValue(arg);
            if (!value) {
                return std::nullopt;
            }
            options.server.bind_address = std::string(*value);
        } else if (arg == "-c" || arg == "--max-clients") {
            const auto value = requireValue(arg);
            if (!value) {
                return std::nullopt;
            }
            const auto count = parseNumber<std::size_t>(*value);
            if (!count || *count == 0) {
                std::cerr << "error: invalid client limit '" << *value << "'\n";
                return std::nullopt;
            }
            options.server.max_clients = *count;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return std::nullopt;
        }
    }

    return options;
}

void printReading(const tbp::ClientInfo& client, const tbp::SensorPayload& reading) {
    std::cout << "[client " << client.id << " " << client.address << "] "
              << "timestamp=" << reading.timestamp << std::fixed << std::setprecision(2)
              << " temperature=" << reading.temperature << " C"
              << " humidity=" << reading.humidity << " %" << std::endl;
}

int runSelfTest() {
    const tbp::SensorPayload sample{static_cast<std::uint32_t>(std::time(nullptr)), 23.75F, 41.5F};
    const auto packet = tbp::PacketParser::createSensorPacket(sample);

    std::cout << "Encoded packet (" << packet.size() << " bytes):";
    for (const std::uint8_t byte : packet) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(byte);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';

    tbp::ParseError error = tbp::ParseError::None;
    const auto decoded = tbp::PacketParser::parseSensorPacket(packet.data(), packet.size(), &error);
    if (!decoded) {
        std::cerr << "self-test failed: " << tbp::toString(error) << '\n';
        return EXIT_FAILURE;
    }

    if (decoded->timestamp != sample.timestamp || decoded->temperature != sample.temperature ||
        decoded->humidity != sample.humidity) {
        std::cerr << "self-test failed: decoded values do not match the original\n";
        return EXIT_FAILURE;
    }

    printReading(tbp::ClientInfo{0, "self-test"}, *decoded);
    std::cout << "Self-test passed\n";
    return EXIT_SUCCESS;
}

int runServer(const tbp::ServerConfig& config) {
    tbp::TcpServer server(config);
    server.setSensorHandler(printReading);
    server.start();

    g_active_server.store(&server);
    installSignalHandlers();

    try {
        server.run();
    } catch (...) {
        g_active_server.store(nullptr);
        throw;
    }

    g_active_server.store(nullptr);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parseArguments(argc, argv);
    if (!options) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (options->show_help) {
        printUsage(argv[0]);
        return EXIT_SUCCESS;
    }

    try {
        if (options->self_test) {
            return runSelfTest();
        }
        return runServer(options->server);
    } catch (const std::system_error& error) {
        std::cerr << "fatal: " << error.what() << " (code " << error.code().value() << ")\n";
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "fatal: unknown error\n";
    }
    return EXIT_FAILURE;
}
