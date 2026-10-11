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
                                 std::optional<std::chrono::nanoseconds> rtt,
                                 std::optional<std::uint64_t> piggyback_ack)
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
            // Piggyback rides on the first fragment only, so the receiver counts the ACK once.
            if (fi == 0 && piggyback_ack.has_value()) header.piggyback_ack_seq = *piggyback_ack;

            auto buf = m_pool->acquire(PACKET_HEADER_WIRE_SIZE + len);
            serialize(header, frag_payload, buf.bytes()).value();

            PendingPacket pending
            {
                .serialized    { std::move(buf) },
                .retransmit    { now + base_timeout },
                .retry_count   { 0 },
                .first_sent_at { now },
            };

            auto [it, inserted] = m_pending.emplace(seq, std::move(pending));
            // Insert matching entry into the deadline-ordered queue, remember our iterator so
            // handle_ack can remove it in O(1) and tick can reinsert on backoff.
            it->second.timer_it = m_retransmit_order.emplace(it->second.retransmit, seq);
            ++m_in_flight;

            wires.emplace_back(it->second.serialized.bytes());

        }

        return wires;

    }

    std::vector<std::span<const std::byte>>
    ReliableOrderedChannel::tick(std::chrono::steady_clock::time_point now,
                                 std::optional<std::chrono::nanoseconds> rtt)
    {

        std::vector<std::span<const std::byte>> retransmits {};
        const auto base_timeout = compute_rto(rtt);

        // Drain only expired entries from the deadline-ordered front — O(log N) per fire
        // instead of the old O(N) scan over m_pending.
        while (!m_retransmit_order.empty() && m_retransmit_order.begin()->first <= now)
        {

            // Extract the top node so we can mutate its key and reinsert it at the new deadline.
            auto node = m_retransmit_order.extract(m_retransmit_order.begin());
            const auto seq = node.mapped();

            auto pend_it = m_pending.find(seq);
            if (pend_it == m_pending.end())
            {
                continue;  // defensive: entry was removed from m_pending but not from queue
            }

            auto& packet = pend_it->second;

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
            const auto new_deadline = now + backoff;
            packet.retransmit = new_deadline;
            retransmits.push_back(packet.serialized.bytes());
            if (m_stats) ++m_stats->retransmits;

            // AIMD multiplicative decrease: every retransmit halves the window, floored at MIN_CWND.
            m_cwnd = std::max(MIN_CWND, m_cwnd / 2u);

            // Reinsert the extracted node at its new deadline; refresh the cached iterator.
            node.key() = new_deadline;
            packet.timer_it = m_retransmit_order.insert(std::move(node));

        }

        return retransmits;

    }

    void ReliableOrderedChannel::handle_ack(std::uint64_t sequence)
    {
        // find-then-erase so we can read first_sent_at before the entry disappears.
        // Duplicate / late ACKs (key missing) must not record a sample or grow cwnd.
        const auto it = m_pending.find(sequence);
        if (it == m_pending.end())
        {
            if (m_stats) ++m_stats->duplicate_acks_ignored;
            return;
        }

        // Sample is end-to-end send→ack from the ORIGINAL send, not the latest retransmit —
        // includes retry time, which is the latency the application actually sees.
        const auto now    = std::chrono::steady_clock::now();
        const auto sample = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now - it->second.first_sent_at);
        m_send_ack_latency.record(sample);

        // Also drop the matching entry from the deadline-ordered queue so tick() won't fire on
        // a packet we've already acked. O(1) because we cached the iterator at send time.
        m_retransmit_order.erase(it->second.timer_it);
        m_pending.erase(it);
        if (m_in_flight > 0) --m_in_flight;
        m_cwnd = std::min(MAX_CWND, m_cwnd + 1u);  // AIMD additive increase
    }

    PacketHeader ReliableOrderedChannel::make_ack(std::uint64_t received_sequence)
    {
        return { received_sequence, PacketType::Ack, CHANNEL_ID, 0 };
    }

} // namespace maroof::net
