#include <maroof/net/PacketHeader.hh>
#include <maroof/net/Crc32.hh>

#include <cstring>
#include <limits>

namespace maroof::net
{

    namespace
    {

        // Manual network-byte-order (big-endian) reads/writes. Keeps this file
        // free of POSIX includes and makes the wire format visible in the code.

        void write_u16_be(std::byte* dst, std::uint16_t v) noexcept
        {
            dst[0] = std::byte((v >> 8) & 0xFF);
            dst[1] = std::byte( v       & 0xFF);
        }

        void write_u32_be(std::byte* dst, std::uint32_t v) noexcept
        {
            dst[0] = std::byte((v >> 24) & 0xFF);
            dst[1] = std::byte((v >> 16) & 0xFF);
            dst[2] = std::byte((v >>  8) & 0xFF);
            dst[3] = std::byte( v        & 0xFF);
        }

        void write_u64_be(std::byte* dst, std::uint64_t v) noexcept
        {
            dst[0] = std::byte((v >> 56) & 0xFF);
            dst[1] = std::byte((v >> 48) & 0xFF);
            dst[2] = std::byte((v >> 40) & 0xFF);
            dst[3] = std::byte((v >> 32) & 0xFF);
            dst[4] = std::byte((v >> 24) & 0xFF);
            dst[5] = std::byte((v >> 16) & 0xFF);
            dst[6] = std::byte((v >>  8) & 0xFF);
            dst[7] = std::byte( v        & 0xFF);
        }

        std::uint16_t read_u16_be(const std::byte* src) noexcept
        {
            return static_cast<std::uint16_t>(
                     (std::to_integer<std::uint16_t>(src[0]) << 8)
                   |  std::to_integer<std::uint16_t>(src[1])
                   );
        }

        std::uint32_t read_u32_be(const std::byte* src) noexcept
        {
            return (std::to_integer<std::uint32_t>(src[0]) << 24)
                 | (std::to_integer<std::uint32_t>(src[1]) << 16)
                 | (std::to_integer<std::uint32_t>(src[2]) <<  8)
                 |  std::to_integer<std::uint32_t>(src[3]);
        }

        std::uint64_t read_u64_be(const std::byte* src) noexcept
        {
            return (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[0])) << 56)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[1])) << 48)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[2])) << 40)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[3])) << 32)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[4])) << 24)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[5])) << 16)
                 | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[6])) <<  8)
                 |  static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[7]));
        }

    } // anonymous namespace

    std::expected<std::size_t, PacketError>
    serialize(const PacketHeader& header,
              std::span<const std::byte> payload,
              std::span<std::byte> out) noexcept
    {
        // Payload length is a u16 on the wire; reject anything that won't fit.
        if (payload.size() > std::numeric_limits<std::uint16_t>::max())
        {
            return std::unexpected(PacketError::PayloadTooLarge);
        }

        const std::size_t total = PACKET_HEADER_WIRE_SIZE + payload.size();
        if (out.size() < total)
        {
            return std::unexpected(PacketError::BufferTooSmall);
        }

        // Lay down the header with the CRC field zeroed, then patch the CRC in
        // once we've hashed everything. Standard "zero, hash, patch" pattern for
        // embedded checksums — the exact same bytes get hashed on both ends.
        write_u64_be(out.data() +  0, header.sequence);
        out[8] = std::byte(static_cast<std::uint8_t>(header.type));
        out[9] = std::byte(header.channel_id);
        write_u16_be(out.data() + 10, static_cast<std::uint16_t>(payload.size()));
        write_u32_be(out.data() + 12, 0u);  // placeholder — CRC patched below

        if (!payload.empty())
        {
            std::memcpy(out.data() + PACKET_HEADER_WIRE_SIZE,
                        payload.data(),
                        payload.size());
        }

        Crc32 crc;
        crc.update(std::span<const std::byte>(out.data(), total));
        write_u32_be(out.data() + 12, crc.value());

        return total;
    }

    std::expected<std::pair<PacketHeader, std::span<const std::byte>>, PacketError>
    parse(std::span<const std::byte> in) noexcept
    {
        if (in.size() < PACKET_HEADER_WIRE_SIZE)
        {
            return std::unexpected(PacketError::TruncatedHeader);
        }

        PacketHeader header;
        header.sequence = read_u64_be(in.data() + 0);

        // Validate the type byte before casting. Casting an out-of-range integer
        // to a scoped enum is legal C++ but produces a value no switch case handles.
        const std::uint8_t type_byte = std::to_integer<std::uint8_t>(in[8]);
        if (!is_valid_packet_type(type_byte))
        {
            return std::unexpected(PacketError::BadType);
        }
        header.type       = static_cast<PacketType>(type_byte);
        header.channel_id = std::to_integer<std::uint8_t>(in[9]);

        // payload_len is read from the wire but not stored on the struct — we
        // expose the payload as a span and callers use span.size() instead.
        const std::uint16_t payload_len = read_u16_be(in.data() + 10);
        header.crc32                    = read_u32_be(in.data() + 12);

        // Guard against a lying length (sender claims more bytes than arrived).
        const std::size_t remaining = in.size() - PACKET_HEADER_WIRE_SIZE;
        if (remaining < payload_len)
        {
            return std::unexpected(PacketError::TruncatedPayload);
        }

        // Verify the CRC. Hash bytes 0..11 (before the CRC field), then four
        // literal zeros in place of the CRC field, then the payload. This must
        // match what serialize() computed.
        Crc32 crc;
        crc.update(in.subspan(0, 12));

        static constexpr std::byte ZERO_CRC[4] {
            std::byte { 0 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 }
        };
        crc.update(std::span<const std::byte>(ZERO_CRC, 4));

        crc.update(in.subspan(PACKET_HEADER_WIRE_SIZE, payload_len));

        if (crc.value() != header.crc32)
        {
            return std::unexpected(PacketError::BadChecksum);
        }

        return std::pair {
            header,
            in.subspan(PACKET_HEADER_WIRE_SIZE, payload_len)
        };
    }

} // namespace maroof::net
