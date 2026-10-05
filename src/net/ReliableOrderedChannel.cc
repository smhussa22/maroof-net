#include <maroof/net/ReliableOrderedChannel.hh>

#include <algorithm>

namespace maroof::net
{

    void ReliableOrderedChannel::seed_receive(std::uint64_t first_seq)
    {
        if (!m_next_expected_seq.has_value())
        {
            m_next_expected_seq = first_seq;
        }
    }

    std::chrono::milliseconds
    ReliableOrderedChannel::compute_rto(std::optional<std::chrono::nanoseconds> rtt) noexcept
    {
        // No RTT sample yet: fall back to the pre-adaptive hardcoded timeout.
        if (!rtt.has_value())
        {
            return INITIAL_TIMEOUT;
        }

        // Classic RTO heuristic: base = 1.5 * smoothed RTT, floored to prevent thrashing
        // on ultra-low-latency links. Exponential backoff is applied on top in tick().
        const auto scaled = std::chrono::duration_cast<std::chrono::milliseconds>(*rtt * 3 / 2);
        return std::max(scaled, MIN_RTO);
    }

    std::span<const std::byte> ReliableOrderedChannel::send(std::span<const std::byte> payload,
                                                            std::optional<std::chrono::nanoseconds> rtt)
    {

        const auto seq { m_next_send_seq++ };
        PacketHeader header { seq, PacketType::Data, CHANNEL_ID, 0 };

        std::vector<std::byte> buf(PACKET_HEADER_WIRE_SIZE + payload.size());
        serialize(header, payload, std::span<std::byte>(buf)).value();

        const auto base_timeout = compute_rto(rtt);
        PendingPacket pending
        {
            .serialized  { std::move(buf) },
            .retransmit  { std::chrono::steady_clock::now() + base_timeout },
            .retry_count { 0 },
        };

        auto [it, inserted] = m_pending.emplace(seq, std::move(pending));

        // AIMD: a fresh pending entry means one more packet is now in flight.
        ++m_in_flight;

        return std::span<const std::byte>(it->second.serialized);

    }

    std::vector<std::span<const std::byte>>
    ReliableOrderedChannel::tick(std::chrono::steady_clock::time_point now,
                                 std::optional<std::chrono::nanoseconds> rtt)
    {

        std::vector<std::span<const std::byte>> retransmits {};
        const auto base_timeout = compute_rto(rtt);

        for (auto& [seq, packet] : m_pending)
        {

            if (packet.retransmit > now)
            {
                continue;
            }

            if (packet.retry_count >= MAX_RETRIES)
            {
                m_dead = true;
                return retransmits;
            }

            packet.retry_count++;

            // Exponential backoff on top of the RTT-derived base. Shift saturates at 4 so the
            // multiplier stays <= 16x the base; MAX_TIMEOUT caps the absolute deadline.
            const std::uint32_t shift = packet.retry_count > 4u ? 4u : packet.retry_count;
            const auto backoff = std::min(base_timeout * (1u << shift), MAX_TIMEOUT);
            packet.retransmit = now + backoff;
            retransmits.push_back(std::span<const std::byte>(packet.serialized));

            // AIMD multiplicative decrease: every retransmit halves the window, floored at MIN_CWND.
            // One halve per retransmit — a storm of drops crushes cwnd fast; a single blip barely dents it.
            m_cwnd = std::max(MIN_CWND, m_cwnd / 2u);

        }

        return retransmits;

    }

    void ReliableOrderedChannel::handle_ack(std::uint64_t sequence)
    {
        // erase returns 1 if the key existed, 0 otherwise. Only adjust AIMD state on a real ACK;
        // duplicate/late ACKs for already-cleared packets must not grow cwnd beyond observed signal.
        if (m_pending.erase(sequence) == 1)
        {
            if (m_in_flight > 0) --m_in_flight;
            m_cwnd = std::min(MAX_CWND, m_cwnd + 1u);  // AIMD additive increase
        }
    }

    PacketHeader ReliableOrderedChannel::make_ack(std::uint64_t received_sequence)
    {
        return { received_sequence, PacketType::Ack, CHANNEL_ID, 0 };
    }

} // namespace maroof::net
