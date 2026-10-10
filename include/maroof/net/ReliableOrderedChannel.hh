#ifndef MAROOF_NET_RELIABLE_ORDERED_CHANNEL_HH
#define MAROOF_NET_RELIABLE_ORDERED_CHANNEL_HH

#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PendingPacket.hh>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace maroof::net
{

class ReliableOrderedChannel
{
public:

    static constexpr std::uint8_t CHANNEL_ID { 0 };

    // Serialize and register a reliable message. Payloads up to MAX_FRAGMENT_PAYLOAD go out as a
    // single packet; larger payloads are split into N fragments, each with its own sequence number
    // and (fragment_id, fragment_count) header fields, each tracked independently for retransmit.
    // Returns one span per fragment, in send order. Spans point into channel-owned storage — send
    // them all before the next call to send/tick/handle_ack.
    // rtt (optional): current smoothed RTT. When present, drives the retransmit deadline via
    // compute_rto; when absent, falls back to the hardcoded INITIAL_TIMEOUT.
    std::vector<std::span<const std::byte>> send(std::span<const std::byte> payload,
                                                 std::optional<std::chrono::nanoseconds> rtt = std::nullopt);

    // Advance retransmit timers. Returns spans to resend; sets dead() on MAX_RETRIES.
    // Caller checks dead() after each call and tears down the Connection if true.
    // rtt feeds the recomputed retransmit deadline on each fire (same contract as send).
    std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now,
                                                 std::optional<std::chrono::nanoseconds> rtt = std::nullopt);

    // Congestion window metrics (AIMD). cwnd and in_flight are exposed for callers that want
    // to gate sends with an application-level queue; this channel itself does NOT block sends.
    // See send/handle_ack/tick for how cwnd evolves (ACK = +1, retransmit = /2, floored at MIN_CWND).
    std::uint32_t cwnd()      const noexcept { return m_cwnd; }
    std::uint32_t in_flight() const noexcept { return m_in_flight; }

    // True once MAX_RETRIES is exhausted on any unacked packet; peer is considered unreachable.
    bool dead() const noexcept { return m_dead; }

    // Remove sequence from pending; no-op on unknown sequence (duplicate or already cleared ACK).
    void handle_ack(std::uint64_t sequence);

    // Build an Ack header carrying received_sequence. Does not consume a send sequence number.
    PacketHeader make_ack(std::uint64_t received_sequence);

    // Seed the receive window to first_seq. No-op if already seeded (duplicate Hello must not reset progress).
    void seed_receive(std::uint64_t first_seq);

    // Dedup + reorder via sliding window. Invokes deliver(span) for each in-order message:
    // for an unfragmented packet (header.fragment_count == 1), fires once with the packet payload;
    // for a fragmented message, accumulates fragments into a reassembly buffer and fires once with
    // the full concatenation when the last fragment slides out of the window.
    // Returns true if the packet was accounted for (delivered, stashed, or a known duplicate) — caller should ACK.
    // Returns false for beyond-window or oversized; caller must NOT ACK so the sender keeps retransmitting.
    // Spans passed to deliver are valid only for that call's duration.
    template <typename OnProcessPayload>
    bool receive(const PacketHeader& header, std::span<const std::byte> payload, OnProcessPayload&& deliver);

private:

    // Retransmit tuning: base timeout doubles per retry (capped at MAX_TIMEOUT).
    // INITIAL_TIMEOUT is the fallback when no RTT sample is available yet.
    // MIN_RTO is the floor for RTT-derived timeouts — prevents thrashing on sub-ms LAN links.
    static constexpr std::uint32_t MAX_RETRIES { 8 };
    static constexpr std::chrono::milliseconds INITIAL_TIMEOUT { 100 };
    static constexpr std::chrono::milliseconds MAX_TIMEOUT { 1600 };
    static constexpr std::chrono::milliseconds MIN_RTO { 20 };

    // AIMD tuning: initial cwnd permits a burst at connection start; MIN floors recovery,
    // MAX bounds growth so a cooperative peer can't balloon cwnd unboundedly over long runs.
    static constexpr std::uint32_t INITIAL_CWND { 10 };
    static constexpr std::uint32_t MIN_CWND     { 1 };
    static constexpr std::uint32_t MAX_CWND     { 1024 };

    // Base retransmit deadline derived from the current RTT estimate (or the hardcoded
    // fallback). Exponential backoff is layered on top in send/tick via retry_count.
    static std::chrono::milliseconds compute_rto(std::optional<std::chrono::nanoseconds> rtt) noexcept;

    // Bounded receive window: bitset for O(1) dedup, slot array for payload storage.
    // Each slot sized to MAX_FRAGMENT_PAYLOAD so slot storage never truncates a fragment.
    static constexpr std::size_t WINDOW_SIZE { 128 };

    // Fragmentation sizing. MAX_FRAGMENT_PAYLOAD is deliberately below a typical path MTU
    // (1500 Ethernet - 20 IPv4 - 8 UDP = 1472) with extra margin for VPN/tunnel overhead,
    // so a serialized fragment (header + payload) stays below the smallest MTU we care about.
    // MAX_MESSAGE_SIZE caps the reassembled payload — bounds the receive-side buffer growth.
    static constexpr std::size_t MAX_FRAGMENT_PAYLOAD { 1200 };
    static constexpr std::size_t MAX_MESSAGE_SIZE    { 64 * 1024 };
    static constexpr std::size_t MAX_PAYLOAD         { MAX_FRAGMENT_PAYLOAD };

    struct ReorderSlot
    {

        std::uint16_t length { 0 };
        std::uint16_t fragment_id { 0 };
        std::uint16_t fragment_count { 1 };  // 1 = unfragmented
        std::array<std::byte, MAX_PAYLOAD> bytes {};

    };

    std::uint64_t m_next_send_seq { 0 };
    bool m_dead { false };
    std::unordered_map<std::uint64_t, PendingPacket> m_pending {};  // in-flight packets awaiting ACK

    // AIMD state. m_in_flight tracks the number of unacked packets (map size + a sanity counter).
    // m_cwnd evolves per ACK (+1, capped at MAX_CWND) and per retransmit (/2, floored at MIN_CWND).
    std::uint32_t m_cwnd      { INITIAL_CWND };
    std::uint32_t m_in_flight { 0 };

    // m_next_expected_seq is unset until seed_receive is called (usually from accept_hello) or
    // until the first packet arrives. m_window_base is the circular index of the front slot.
    std::optional<std::uint64_t> m_next_expected_seq {};
    std::size_t m_window_base { 0 };
    std::bitset<WINDOW_SIZE> m_received {};
    std::array<ReorderSlot, WINDOW_SIZE> m_slots {};

    // Reassembly scratch for fragmented messages. Fragments arrive in sequence order
    // (window guarantee), so we just append each one as it drains and fire deliver()
    // once when the final fragment lands.
    std::vector<std::byte> m_reassembly_buf {};

};

template <typename OnProcessPayload>
bool ReliableOrderedChannel::receive(const PacketHeader& header, std::span<const std::byte> payload, OnProcessPayload&& deliver)
{

    const std::uint64_t sequence = header.sequence;

    if (!m_next_expected_seq.has_value())
    {
        m_next_expected_seq = sequence;
    }

    const std::uint64_t next = *m_next_expected_seq;

    // Below window: already delivered. ACK again — our earlier ACK was probably lost.
    if (sequence < next)
    {
        return true;
    }

    // Beyond window: no slot available. Do NOT ACK — sender must keep retrying until
    // the window slides far enough to accept it. ACKing here would drop the packet forever.
    if (sequence >= next + WINDOW_SIZE)
    {
        return false;
    }

    // Payload wouldn't fit a slot — same as beyond-window: can't track it.
    if (payload.size() > MAX_PAYLOAD)
    {
        return false;
    }

    // fragment_count == 0 is a malformed header (unfragmented must be 1). Treat as untrackable.
    if (header.fragment_count == 0 || header.fragment_id >= header.fragment_count)
    {
        return false;
    }

    const std::size_t slot_idx = (m_window_base + (sequence - next)) % WINDOW_SIZE;

    // Duplicate: bit already set. Already accounted for, ACK again.
    if (m_received.test(slot_idx))
    {
        return true;
    }

    // Stash into the slot.
    auto& slot = m_slots[slot_idx];
    slot.length         = static_cast<std::uint16_t>(payload.size());
    slot.fragment_id    = header.fragment_id;
    slot.fragment_count = header.fragment_count;
    std::memcpy(slot.bytes.data(), payload.data(), payload.size());
    m_received.set(slot_idx);

    // If not at the front of the window, we're done — just stashed.
    if (sequence != next)
    {
        return true;
    }

    // At the front. Drain consecutive slots and slide the window.
    while (m_received.test(m_window_base))
    {
        auto& front = m_slots[m_window_base];
        std::span<const std::byte> frag { front.bytes.data(), front.length };

        if (front.fragment_count == 1)
        {
            // Unfragmented message — deliver straight to the user.
            deliver(frag);
        }
        else
        {
            // Fragmented. Accumulate; fire once on the terminal fragment.
            m_reassembly_buf.insert(m_reassembly_buf.end(), frag.begin(), frag.end());
            if (front.fragment_id == front.fragment_count - 1)
            {
                deliver(std::span<const std::byte>(m_reassembly_buf));
                m_reassembly_buf.clear();
            }
        }

        m_received.reset(m_window_base);
        m_next_expected_seq = *m_next_expected_seq + 1;
        m_window_base = (m_window_base + 1) % WINDOW_SIZE;
    }

    return true;

}

} // namespace maroof::net

#endif // MAROOF_NET_RELIABLE_ORDERED_CHANNEL_HH
