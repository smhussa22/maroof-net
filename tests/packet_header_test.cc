#include <gtest/gtest.h>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PacketType.hh>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

using namespace maroof::net;

namespace
{
    std::span<const std::byte> as_bytes(const char* s, std::size_t n)
    {
        return { reinterpret_cast<const std::byte*>(s), n };
    }
}

TEST(PacketHeaderTest, RoundTripWithPayload)
{
    PacketHeader h { 42, PacketType::Data, 3, 0 };
    const char* msg = "hello world";
    const auto payload = as_bytes(msg, std::strlen(msg));

    std::array<std::byte, 128> buf {};
    auto written = serialize(h, payload, std::span<std::byte>(buf));
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), PACKET_HEADER_WIRE_SIZE + std::strlen(msg));

    auto parsed = parse(std::span<const std::byte>(buf.data(), written.value()));
    ASSERT_TRUE(parsed.has_value());

    const auto& [header, out_payload] = parsed.value();
    EXPECT_EQ(header.sequence,   42u);
    EXPECT_EQ(header.type,       PacketType::Data);
    EXPECT_EQ(header.channel_id, 3u);
    EXPECT_EQ(out_payload.size(), std::strlen(msg));
    EXPECT_EQ(std::memcmp(out_payload.data(), msg, out_payload.size()), 0);
}

TEST(PacketHeaderTest, RoundTripEmptyPayload)
{
    PacketHeader h { 7, PacketType::Hello, 0, 0 };
    std::array<std::byte, 32> buf {};

    auto written = serialize(h, {}, std::span<std::byte>(buf));
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written.value(), PACKET_HEADER_WIRE_SIZE);

    auto parsed = parse(std::span<const std::byte>(buf.data(), written.value()));
    ASSERT_TRUE(parsed.has_value());

    const auto& [header, payload] = parsed.value();
    EXPECT_EQ(header.type, PacketType::Hello);
    EXPECT_EQ(payload.size(), 0u);
}

TEST(PacketHeaderTest, SerializeFailsIfBufferTooSmall)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    const char* msg = "hello";
    const auto payload = as_bytes(msg, std::strlen(msg));

    std::array<std::byte, 10> tiny_buf {};  // header alone is 20 bytes
    auto result = serialize(h, payload, std::span<std::byte>(tiny_buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::BufferTooSmall);
}

TEST(PacketHeaderTest, ParseFailsOnTruncatedHeader)
{
    std::array<std::byte, 8> tiny_buf {};  // less than 20
    auto result = parse(std::span<const std::byte>(tiny_buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::TruncatedHeader);
}

TEST(PacketHeaderTest, ParseFailsOnBadType)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    std::array<std::byte, 32> buf {};
    auto written = serialize(h, {}, std::span<std::byte>(buf)).value();

    // Corrupt the type byte at offset 8.
    buf[8] = std::byte { 99 };

    auto result = parse(std::span<const std::byte>(buf.data(), written));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::BadType);
}

TEST(PacketHeaderTest, ParseFailsOnBadChecksum)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    const char* msg = "hello";
    const auto payload = as_bytes(msg, std::strlen(msg));

    std::array<std::byte, 32> buf {};
    auto written = serialize(h, payload, std::span<std::byte>(buf)).value();

    // Flip the last payload byte to guarantee a CRC mismatch.
    buf[written - 1] = std::byte { static_cast<std::uint8_t>(
        ~std::to_integer<std::uint8_t>(buf[written - 1])) };

    auto result = parse(std::span<const std::byte>(buf.data(), written));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::BadChecksum);
}

TEST(PacketHeaderTest, ParseFailsOnTruncatedPayload)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    const char* msg = "hello";
    const auto payload = as_bytes(msg, std::strlen(msg));

    std::array<std::byte, 32> buf {};
    auto written = serialize(h, payload, std::span<std::byte>(buf)).value();

    // Chop off part of the payload — wire length still claims the full amount.
    auto result = parse(std::span<const std::byte>(buf.data(), written - 2));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::TruncatedPayload);
}

TEST(PacketHeaderTest, SequenceRoundTripsFullU64)
{
    // Verify the u64 sequence survives serialize/parse across a value that
    // would truncate on a shorter integer width.
    PacketHeader h { 0xDEADBEEFCAFEBABEull, PacketType::Data, 0, 0 };
    std::array<std::byte, 32> buf {};

    auto written = serialize(h, {}, std::span<std::byte>(buf)).value();
    auto parsed  = parse(std::span<const std::byte>(buf.data(), written)).value();
    EXPECT_EQ(parsed.first.sequence, 0xDEADBEEFCAFEBABEull);
}

TEST(PacketHeaderTest, ChannelIdRoundTrips)
{
    for (std::uint8_t ch : { std::uint8_t{0}, std::uint8_t{7}, std::uint8_t{255} })
    {
        PacketHeader h { 1, PacketType::Data, ch, 0 };
        std::array<std::byte, 32> buf {};
        auto written = serialize(h, {}, std::span<std::byte>(buf)).value();
        auto parsed  = parse(std::span<const std::byte>(buf.data(), written)).value();
        EXPECT_EQ(parsed.first.channel_id, ch);
    }
}

TEST(PacketHeaderTest, FragmentFieldsRoundTrip)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    h.fragment_id    = 7;
    h.fragment_count = 42;

    std::array<std::byte, 32> buf {};
    auto written = serialize(h, {}, std::span<std::byte>(buf)).value();
    auto parsed  = parse(std::span<const std::byte>(buf.data(), written)).value();

    EXPECT_EQ(parsed.first.fragment_id,    7u);
    EXPECT_EQ(parsed.first.fragment_count, 42u);
}

TEST(PacketHeaderTest, FragmentFieldsCoveredByCrc)
{
    PacketHeader h { 1, PacketType::Data, 0, 0 };
    h.fragment_id    = 1;
    h.fragment_count = 3;

    std::array<std::byte, 32> buf {};
    auto written = serialize(h, {}, std::span<std::byte>(buf)).value();

    // Tamper with the fragment_count byte at offset 18 — must trip the checksum.
    buf[18] = std::byte { static_cast<std::uint8_t>(
        ~std::to_integer<std::uint8_t>(buf[18])) };

    auto result = parse(std::span<const std::byte>(buf.data(), written));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PacketError::BadChecksum);
}
