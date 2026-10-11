#ifndef MAROOF_NET_PENDING_PACKET_HH
#define MAROOF_NET_PENDING_PACKET_HH

#include <maroof/net/PacketPool.hh>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>

namespace maroof::net
{

    // Ordered-by-deadline index used by the channel's tick() so draining expired packets is
    // O(log N) per fire instead of O(N) over m_pending. Multimap keeps iterators stable across
    // other insertions/erasures, which lets PendingPacket cache its own entry.
    using RetransmitQueue = std::multimap<std::chrono::steady_clock::time_point, std::uint64_t>;

    struct PendingPacket
    {

        PooledBuffer serialized {};                             // pool-backed wire bytes; slot returns on destruction
        std::chrono::steady_clock::time_point retransmit {};    // next retransmit deadline for this unACKed packet
        uint32_t retry_count {};                                // enforces per-packet retry cap
        std::chrono::steady_clock::time_point first_sent_at {}; // original send timestamp; survives retransmits so send→ack latency includes retry time
        RetransmitQueue::iterator timer_it {};                  // this packet's entry in the channel's deadline-ordered retransmit queue (O(1) removal)

    };
    

} // namespace maroof::net

#endif // MAROOF_NET_PENDING_PACKET_HH