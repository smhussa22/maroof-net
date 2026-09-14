#ifndef MAROOF_NET_PACKET_TYPE_HH
#define MAROOF_NET_PACKET_TYPE_HH

#include <cstdint>

namespace maroof::net
{

    // One-byte discriminator carried in every PacketHeader.
    //
    // Reliability semantics are NOT encoded here — they're a property of the channel
    // (see PacketHeader::channel_id + Channel setup at handshake time). 
    // Every payload-carrying packet is just "Data".
    // The receiver looks up the channel decide whether to ACK it, drop it if stale, deliver in order, etc.

    // ***NOTE:*** if this enum starts to get large i  will reconsider whether or not extra command types could be implemented as
    // channel behaviour instead.
    enum class PacketType : std::uint8_t
    {
        None       = 0,   // Sentinel for uninitialized / invalid; never sent on the wire.
        Hello      = 1,   // Client -> server: request to establish a connection.
        HelloAck   = 2,   // Server -> client: connection accepted.
        Data       = 3,   // Payload-carrying packet; channel_id decides how to handle it.
        Ack        = 4,   // Acknowledgment for a reliable Data packet on some channel.
        Disconnect = 5,   // Graceful shutdown notice; peer may free connection state.
    };

    // True if `v` corresponds to a defined PacketType above (excluding None, which
    // must never appear on the wire). Use at the parse boundary to reject junk
    // before dispatching on `type`.
    inline constexpr bool is_valid_packet_type(std::uint8_t v) noexcept
    {

        return v >= static_cast<std::uint8_t>(PacketType::Hello) && v <= static_cast<std::uint8_t>(PacketType::Disconnect);
    
    }

} // namespace maroof::net

#endif // MAROOF_NET_PACKET_TYPE_HH
