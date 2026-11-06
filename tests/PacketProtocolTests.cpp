#include "PacketProtocol.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(condition)                                                          \
    do {                                                                                \
        if (!(condition)) {                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected " #condition "\n"; \
            ++g_failures;                                                               \
        }                                                                               \
    } while (false)

using tbp::PacketParser;
using tbp::ParseError;
using tbp::SensorPayload;

std::vector<std::uint8_t> samplePacket() {
    const auto packet = PacketParser::createSensorPacket(SensorPayload{1700000000U, 21.5F, 55.25F});
    return {packet.begin(), packet.end()};
}

void testRoundTrip() {
    const SensorPayload original{1700000000U, -12.5F, 87.75F};
    const auto packet = PacketParser::createSensorPacket(original);

    ParseError error = ParseError::BufferTooSmall;
    const auto decoded = PacketParser::parseSensorPacket(packet.data(), packet.size(), &error);

    EXPECT_TRUE(decoded.has_value());
    EXPECT_TRUE(error == ParseError::None);
    if (decoded) {
        EXPECT_TRUE(decoded->timestamp == original.timestamp);
        EXPECT_TRUE(decoded->temperature == original.temperature);
        EXPECT_TRUE(decoded->humidity == original.humidity);
    }
}

void testWireLayoutIsLittleEndian() {
    const auto packet = PacketParser::createSensorPacket(SensorPayload{0x11223344U, 0.0F, 0.0F});

    EXPECT_TRUE(packet[0] == tbp::protocol::kSyncByte);
    EXPECT_TRUE(packet[1] == static_cast<std::uint8_t>(tbp::CommandId::SensorData));
    EXPECT_TRUE(packet[2] == 12 && packet[3] == 0);
    EXPECT_TRUE(packet[4] == 0x44 && packet[5] == 0x33 && packet[6] == 0x22 && packet[7] == 0x11);
}

void testRejectsNullBuffer() {
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::parseSensorPacket(nullptr, 16, &error));
    EXPECT_TRUE(error == ParseError::NullBuffer);
}

void testRejectsShortBuffers() {
    const auto packet = samplePacket();
    for (std::size_t size = 0; size < packet.size(); ++size) {
        ParseError error = ParseError::None;
        EXPECT_TRUE(!PacketParser::parseSensorPacket(packet.data(), size, &error));
        EXPECT_TRUE(error == ParseError::BufferTooSmall);
    }
}

void testRejectsTrailingBytes() {
    auto packet = samplePacket();
    packet.push_back(0x00);
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::parseSensorPacket(packet, &error));
    EXPECT_TRUE(error == ParseError::TrailingBytes);
}

void testRejectsInvalidSyncByte() {
    auto packet = samplePacket();
    packet[0] = 0x55;
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::parseSensorPacket(packet, &error));
    EXPECT_TRUE(error == ParseError::InvalidSyncByte);
}

void testRejectsUnexpectedCommand() {
    auto packet = samplePacket();
    packet[1] = 0x7F;
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::parseSensorPacket(packet, &error));
    EXPECT_TRUE(error == ParseError::UnexpectedCommand);
}

void testRejectsLengthMismatch() {
    auto packet = samplePacket();
    packet[2] = 8;
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::parseSensorPacket(packet, &error));
    EXPECT_TRUE(error == ParseError::PayloadLengthMismatch);
}

void testRejectsOversizedPayload() {
    auto packet = samplePacket();
    packet[2] = 0xFF;
    packet[3] = 0xFF;
    ParseError error = ParseError::None;
    EXPECT_TRUE(!PacketParser::frameSize(packet.data(), packet.size(), &error));
    EXPECT_TRUE(error == ParseError::PayloadTooLarge);
}

void testRejectsImplausibleValues() {
    const float bad_values[] = {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(), 500.0F, -150.0F};
    for (const float value : bad_values) {
        const auto packet = PacketParser::createSensorPacket(SensorPayload{1U, value, 50.0F});
        ParseError error = ParseError::None;
        EXPECT_TRUE(!PacketParser::parseSensorPacket(packet.data(), packet.size(), &error));
        EXPECT_TRUE(error == ParseError::ValueOutOfRange);
    }

    const auto humid = PacketParser::createSensorPacket(SensorPayload{1U, 20.0F, 100.5F});
    EXPECT_TRUE(!PacketParser::parseSensorPacket(humid.data(), humid.size()));
}

void testFrameSize() {
    const auto packet = samplePacket();
    ParseError error = ParseError::None;

    EXPECT_TRUE(!PacketParser::frameSize(packet.data(), 3, &error));
    EXPECT_TRUE(error == ParseError::BufferTooSmall);

    const auto size = PacketParser::frameSize(packet.data(), packet.size(), &error);
    EXPECT_TRUE(size.has_value() && *size == tbp::protocol::kSensorPacketSize);
}

}  // namespace

int main() {
    testRoundTrip();
    testWireLayoutIsLittleEndian();
    testRejectsNullBuffer();
    testRejectsShortBuffers();
    testRejectsTrailingBytes();
    testRejectsInvalidSyncByte();
    testRejectsUnexpectedCommand();
    testRejectsLengthMismatch();
    testRejectsOversizedPayload();
    testRejectsImplausibleValues();
    testFrameSize();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All PacketProtocol tests passed\n";
    return EXIT_SUCCESS;
}
