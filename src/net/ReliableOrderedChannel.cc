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

    std::vector<std::span<const std::byte>>
    ReliableOrderedChannel::send(std::span<const std::byte> payload,
                                 std::optional<std::chrono::nanoseconds> rtt)
    {

        // Split payload into N fragments of at most MAX_FRAGMENT_PAYLOAD each. A zero-byte
        // payload still goes out as one "fragment" with fragment_count = 1 so the handshake
        // of {empty payload reliable send} isn't a special case anywhere else.
        const std::size_t total = payload.size();
        const std::size_t fc    = (total == 0)
                                      ? 1
                                      : (total + MAX_FRAGMENT_PAYLOAD - 1) / MAX_FRAGMENT_PAYLOAD;

        const auto base_timeout = compute_rto(rtt);
        const auto now          = std::chrono::steady_clock::now();

        std::vector<std::span<const std::byte>> wires;
        wires.reserve(fc);

        for (std::size_t fi = 0; fi < fc; ++fi)
        {

            const std::size_t off = fi * MAX_FRAGMENT_PAYLOAD;
            const std::size_t len = (total == 0)
                                        ? 0
                                        : std::min(MAX_FRAGMENT_PAYLOAD, total - off);
            auto frag_payload = payload.subspan(off, len);

            const auto seq = m_next_send_seq++;
            PacketHeader header { seq, PacketType::Data, CHANNEL_ID, 0 };
            header.fragment_id    = static_cast<std::uint16_t>(fi);
            header.fragment_count = static_cast<std::uint16_t>(fc);

            std::vector<std::byte> buf(PACKET_HEADER_WIRE_SIZE + len);
            serialize(header, frag_payload, std::span<std::byte>(buf)).value();

            PendingPacket pending
            {
                .serialized    { std::move(buf) },
                .retransmit    { now + base_timeout },
                .retry_count   { 0 },
                .first_sent_at { now },
            };

            auto [it, inserted] = m_pending.emplace(seq, std::move(pending));
            ++m_in_flight;

            wires.emplace_back(std::span<const std::byte>(it->second.serialized));

        }

        return wires;

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
        // find-then-erase so we can read first_sent_at before the entry disappears.
        // Duplicate / late ACKs (key missing) must not record a sample or grow cwnd.
        const auto it = m_pending.find(sequence);
        if (it == m_pending.end()) return;

        // Sample is end-to-end send→ack from the ORIGINAL send, not the latest retransmit —
        // includes retry time, which is the latency the application actually sees.
        const auto now    = std::chrono::steady_clock::now();
        const auto sample = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now - it->second.first_sent_at);
        m_send_ack_latency.record(sample);

        m_pending.erase(it);
        if (m_in_flight > 0) --m_in_flight;
        m_cwnd = std::min(MAX_CWND, m_cwnd + 1u);  // AIMD additive increase
    }

    PacketHeader ReliableOrderedChannel::make_ack(std::uint64_t received_sequence)
    {
        return { received_sequence, PacketType::Ack, CHANNEL_ID, 0 };
    }

} // namespace maroof::net
