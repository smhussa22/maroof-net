#ifndef MAROOF_NET_PACKET_HEADER_HH
#define MAROOF_NET_PACKET_HEADER_HH

#include <maroof/net/PacketType.hh>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace maroof::net
{

    // Sentinel for "no piggybacked ACK on this packet." u64::max is safe because real sequence
    // counters start at 0 and would need ~584 years at 1 billion sends/sec to overflow.
    inline constexpr std::uint64_t NO_PIGGYBACK_ACK { 0xFFFF'FFFF'FFFF'FFFFULL };

    // In-memory header for a single packet. The wire form is produced by
    // serialize and consumed by parse. We never memcpy this struct to or from
    // the wire because that would depend on host byte order and compiler
    // padding, both hostile to portability.
    //
    // Wire layout is 28 bytes, all multi-byte fields in network byte order:
    //   bytes  0..7  : sequence (u64, per-connection-per-channel)
    //   byte      8  : type (PacketType, never None on the wire)
    //   byte      9  : channel_id
    //   bytes 10..11 : payload_len (wire only, not on this struct)
    //   bytes 12..15 : crc32 over header-with-crc-field-zeroed plus payload
    //   bytes 16..17 : fragment_id    (0..fragment_count-1)
    //   bytes 18..19 : fragment_count (1 for unfragmented packets)
    //   bytes 20..27 : piggyback_ack_seq (u64; NO_PIGGYBACK_ACK = none)
    //
    // payload_len lives on the wire but not on the struct. On send, serialize
    // writes it from payload.size(). On receive, parse returns the payload as
    // a std::span so you get the length from span.size().
    struct PacketHeader
    {

        std::uint64_t sequence { 0 };
        PacketType type { PacketType::None };
        std::uint8_t  channel_id { 0 };
        std::uint32_t crc32 { 0 };
        std::uint16_t fragment_id { 0 };
        std::uint16_t fragment_count { 1 };          // 1 = unfragmented (default for every non-reliable-data packet)
        std::uint64_t piggyback_ack_seq { NO_PIGGYBACK_ACK };  // non-sentinel = "I'm also ACKing this sequence"

    };

    // Size of the header on the wire. Not sizeof(PacketHeader), which reflects
    // compiler padding and doesn't match the serialized form.
    inline constexpr std::size_t PACKET_HEADER_WIRE_SIZE = 28;

    enum class PacketError : std::uint8_t
    {
        BufferTooSmall,    // serialize: output buffer can't hold header + payload.
        PayloadTooLarge,   // serialize: payload.size() exceeds the u16 wire length field.
        TruncatedHeader,   // parse: input has fewer than PACKET_HEADER_WIRE_SIZE bytes.
        TruncatedPayload,  // parse: wire length says N but fewer than N bytes remain.
        BadType,           // parse: type byte isn't a defined PacketType value.
        BadChecksum,       // parse: computed CRC doesn't match the one in the header.
    };

    // Writes header and payload into out. Computes CRC and payload length
    // internally. Returns bytes written on success.
    std::expected<size_t, PacketError>
    serialize(const PacketHeader& header, std::span<const std::byte> payload, std::span<std::byte> out) noexcept;

    // Parses in and returns {header, payload span}. The payload span aliases
    // into in, so it's only valid as long as in's storage is.
    std::expected<std::pair<PacketHeader, std::span<const std::byte>>, PacketError>
    parse(std::span<const std::byte> in) noexcept;

} // namespace maroof::net

#endif // MAROOF_NET_PACKET_HEADER_HH