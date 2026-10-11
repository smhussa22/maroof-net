#ifndef MAROOF_NET_PENDING_PACKET_HH
#define MAROOF_NET_PENDING_PACKET_HH

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace maroof::net
{

    struct PendingPacket
    {

        std::vector<std::byte> serialized {};                   // ful wire bytes (header and payload) that are ready to send or re send
        std::chrono::steady_clock::time_point retransmit {};    // the time to retransmit if the packet is still unACKed
        uint32_t retry_count {};                                // enforces per packet retry cap.
        std::chrono::steady_clock::time_point first_sent_at {}; // original send timestamp; survives retransmits so send→ack latency includes retry time

    };
    

} // namespace maroof::net

#endif // MAROOF_NET_PENDING_PACKET_HH