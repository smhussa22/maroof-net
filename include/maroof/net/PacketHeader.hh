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

    // In-memory header for a single packet. The wire form is produced by
    // serialize and consumed by parse. We never memcpy this struct to or from
    // the wire because that would depend on host byte order and compiler
    // padding, both hostile to portability.
    //
    // Wire layout is 16 bytes, all multi-byte fields in network byte order:
    // 8 bytes sequence (u64, per-connection-per-channel), 1 byte type
    // (PacketType, never None on the wire), 1 byte channel_id, 2 bytes
    // payload_len (wire only, not on this struct), 4 bytes crc32 (over
    // header-with-crc-field-zeroed plus payload).
    //
    // payload_len lives on the wire but not on the struct. On send, serialize
    // writes it from payload.size(). On receive, parse returns the payload as
    // a std::span so you get the length from span.size(). No struct field to
    // set on send or read on receive, so no "same field means different things
    // depending on direction" confusion.
    struct PacketHeader
    {

        std::uint64_t sequence { 0 };
        PacketType type { PacketType::None };
        std::uint8_t  channel_id { 0 };
        std::uint32_t crc32 { 0 };

        // TODO (fragmentation, Phase 3 pending): add two u16 fields and bump
        // PACKET_HEADER_WIRE_SIZE from 16 to 20. Design:
        //   std::uint16_t fragment_id    { 0 };  // 0..fragment_count-1
        //   std::uint16_t fragment_count { 1 };  // 1 = unfragmented (default for all current packets)
        // Window delivers in sequence order, so fragments of one message arrive contiguously.
        // Receive side accumulates into a reassembly buffer until fragment_id == fragment_count-1,
        // then calls deliver() once with the concatenated payload.
        // Only ReliableOrderedChannel fragments; unreliable channels drop if payload > MTU.
        // Serialize/parse (src/net/PacketHeader.cc) must learn the two new u16s between
        // the current CRC field and the payload — or wedge them in before payload_len.
        // Pick the layout, update PACKET_HEADER_WIRE_SIZE, then propagate to all serialize callers.

    };

    // Size of the header on the wire. Not sizeof(PacketHeader), which reflects
    // compiler padding and doesn't match the serialized form.
    inline constexpr std::size_t PACKET_HEADER_WIRE_SIZE = 16;  // TODO: bump to 20 when fragmentation lands.

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