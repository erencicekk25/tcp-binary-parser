#ifndef TCP_BINARY_PARSER_PACKET_PROTOCOL_HPP
#define TCP_BINARY_PARSER_PACKET_PROTOCOL_HPP

// Header-only binary packet protocol.
//
// Wire format (all multi-byte fields are little-endian, independent of host):
//
//   +-----------+--------+-------------+-------------------------+
//   | sync_byte | cmd_id | payload_len | payload (payload_len B) |
//   |   1 byte  | 1 byte |   2 bytes   |                         |
//   +-----------+--------+-------------+-------------------------+
//
// SensorPayload (cmd_id = 0x01, 12 bytes):
//   timestamp   : uint32  (seconds, e.g. Unix epoch)
//   temperature : float32 (IEEE-754, degrees Celsius)
//   humidity    : float32 (IEEE-754, relative humidity in percent)

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace tbp {

static_assert(std::numeric_limits<float>::is_iec559,
              "PacketProtocol requires IEEE-754 single precision floats");
static_assert(sizeof(float) == sizeof(std::uint32_t),
              "PacketProtocol requires 32-bit floats");

#pragma pack(push, 1)

struct PacketHeader {
    std::uint8_t sync_byte;
    std::uint8_t cmd_id;
    std::uint16_t payload_len;
};

struct SensorPayload {
    std::uint32_t timestamp;
    float temperature;
    float humidity;
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 4, "PacketHeader must be 4 bytes on the wire");
static_assert(sizeof(SensorPayload) == 12, "SensorPayload must be 12 bytes on the wire");

namespace protocol {

inline constexpr std::uint8_t kSyncByte = 0xAA;
inline constexpr std::size_t kHeaderSize = sizeof(PacketHeader);
inline constexpr std::size_t kSensorPayloadSize = sizeof(SensorPayload);
inline constexpr std::size_t kSensorPacketSize = kHeaderSize + kSensorPayloadSize;

// Upper bound for any payload accepted by the parser. Keeps a corrupted
// length field from forcing a receiver to buffer large amounts of data.
inline constexpr std::size_t kMaxPayloadSize = 1024;

inline constexpr float kMinTemperature = -100.0F;
inline constexpr float kMaxTemperature = 200.0F;
inline constexpr float kMinHumidity = 0.0F;
inline constexpr float kMaxHumidity = 100.0F;

}  // namespace protocol

enum class CommandId : std::uint8_t {
    SensorData = 0x01,
};

enum class ParseError : std::uint8_t {
    None,
    NullBuffer,
    BufferTooSmall,
    InvalidSyncByte,
    UnexpectedCommand,
    PayloadLengthMismatch,
    PayloadTooLarge,
    TrailingBytes,
    ValueOutOfRange,
};

constexpr const char* toString(ParseError error) noexcept {
    switch (error) {
        case ParseError::None: return "none";
        case ParseError::NullBuffer: return "null buffer";
        case ParseError::BufferTooSmall: return "buffer too small";
        case ParseError::InvalidSyncByte: return "invalid sync byte";
        case ParseError::UnexpectedCommand: return "unexpected command id";
        case ParseError::PayloadLengthMismatch: return "payload length mismatch";
        case ParseError::PayloadTooLarge: return "payload too large";
        case ParseError::TrailingBytes: return "trailing bytes after packet";
        case ParseError::ValueOutOfRange: return "sensor value out of range";
    }
    return "unknown";
}

namespace detail {

inline void writeU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

inline void writeU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

inline void writeF32(std::uint8_t* out, float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(out, bits);
}

inline std::uint16_t readU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                      static_cast<std::uint16_t>(in[1] << 8U));
}

inline std::uint32_t readU32(const std::uint8_t* in) noexcept {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8U) |
           (static_cast<std::uint32_t>(in[2]) << 16U) |
           (static_cast<std::uint32_t>(in[3]) << 24U);
}

inline float readF32(const std::uint8_t* in) noexcept {
    const std::uint32_t bits = readU32(in);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void setError(ParseError* error, ParseError value) noexcept {
    if (error != nullptr) {
        *error = value;
    }
}

}  // namespace detail

class PacketParser {
public:
    using SensorPacket = std::array<std::uint8_t, protocol::kSensorPacketSize>;

    // Serializes a sensor reading into a complete, ready-to-send packet.
    static SensorPacket createSensorPacket(const SensorPayload& payload) noexcept {
        SensorPacket packet{};
        std::uint8_t* out = packet.data();

        out[0] = protocol::kSyncByte;
        out[1] = static_cast<std::uint8_t>(CommandId::SensorData);
        detail::writeU16(out + 2, static_cast<std::uint16_t>(protocol::kSensorPayloadSize));

        std::uint8_t* body = out + protocol::kHeaderSize;
        detail::writeU32(body + 0, payload.timestamp);
        detail::writeF32(body + 4, payload.temperature);
        detail::writeF32(body + 8, payload.humidity);

        return packet;
    }

    // Decodes the header at the start of `data`. Only the sync byte and the
    // payload length limit are validated here; the command id is not checked.
    static std::optional<PacketHeader> parseHeader(const std::uint8_t* data, std::size_t size,
                                                   ParseError* error = nullptr) noexcept {
        if (data == nullptr) {
            detail::setError(error, ParseError::NullBuffer);
            return std::nullopt;
        }
        if (size < protocol::kHeaderSize) {
            detail::setError(error, ParseError::BufferTooSmall);
            return std::nullopt;
        }
        if (data[0] != protocol::kSyncByte) {
            detail::setError(error, ParseError::InvalidSyncByte);
            return std::nullopt;
        }

        PacketHeader header{};
        header.sync_byte = data[0];
        header.cmd_id = data[1];
        header.payload_len = detail::readU16(data + 2);

        if (header.payload_len > protocol::kMaxPayloadSize) {
            detail::setError(error, ParseError::PayloadTooLarge);
            return std::nullopt;
        }

        detail::setError(error, ParseError::None);
        return header;
    }

    // Parses exactly one sensor packet. `size` must match the packet size;
    // extra bytes are rejected so that framing errors are not silently hidden.
    static std::optional<SensorPayload> parseSensorPacket(const std::uint8_t* data, std::size_t size,
                                                          ParseError* error = nullptr) noexcept {
        const std::optional<PacketHeader> header = parseHeader(data, size, error);
        if (!header) {
            return std::nullopt;
        }
        if (header->cmd_id != static_cast<std::uint8_t>(CommandId::SensorData)) {
            detail::setError(error, ParseError::UnexpectedCommand);
            return std::nullopt;
        }
        if (header->payload_len != protocol::kSensorPayloadSize) {
            detail::setError(error, ParseError::PayloadLengthMismatch);
            return std::nullopt;
        }
        if (size < protocol::kSensorPacketSize) {
            detail::setError(error, ParseError::BufferTooSmall);
            return std::nullopt;
        }
        if (size > protocol::kSensorPacketSize) {
            detail::setError(error, ParseError::TrailingBytes);
            return std::nullopt;
        }

        const std::uint8_t* body = data + protocol::kHeaderSize;
        SensorPayload payload{};
        payload.timestamp = detail::readU32(body + 0);
        payload.temperature = detail::readF32(body + 4);
        payload.humidity = detail::readF32(body + 8);

        if (!isPlausible(payload)) {
            detail::setError(error, ParseError::ValueOutOfRange);
            return std::nullopt;
        }

        detail::setError(error, ParseError::None);
        return payload;
    }

    static std::optional<SensorPayload> parseSensorPacket(const std::vector<std::uint8_t>& buffer,
                                                          ParseError* error = nullptr) noexcept {
        return parseSensorPacket(buffer.data(), buffer.size(), error);
    }

    // Returns the full frame size (header + payload) announced by a valid
    // header at the start of `data`, or std::nullopt if the header is not
    // complete yet or is invalid (check `error` to tell the two apart).
    static std::optional<std::size_t> frameSize(const std::uint8_t* data, std::size_t size,
                                                ParseError* error = nullptr) noexcept {
        const std::optional<PacketHeader> header = parseHeader(data, size, error);
        if (!header) {
            return std::nullopt;
        }
        return protocol::kHeaderSize + header->payload_len;
    }

private:
    static bool isPlausible(const SensorPayload& payload) noexcept {
        return std::isfinite(payload.temperature) && std::isfinite(payload.humidity) &&
               payload.temperature >= protocol::kMinTemperature &&
               payload.temperature <= protocol::kMaxTemperature &&
               payload.humidity >= protocol::kMinHumidity &&
               payload.humidity <= protocol::kMaxHumidity;
    }
};

}  // namespace tbp

#endif  // TCP_BINARY_PARSER_PACKET_PROTOCOL_HPP
