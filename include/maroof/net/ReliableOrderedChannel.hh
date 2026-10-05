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

    // Serialize and register a reliable packet. Returns wire bytes to send immediately;
    // span points into storage owned by this channel — send before the next call to send/tick/handle_ack.
    // rtt (optional): current smoothed RTT. When present, drives the retransmit deadline via
    // compute_rto; when absent, falls back to the hardcoded INITIAL_TIMEOUT. Default std::nullopt
    // preserves pre-adaptive behavior for callers that don't have an RTT sample yet.
    std::span<const std::byte> send(std::span<const std::byte> payload,
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

    // Dedup + reorder via sliding window. Invokes deliver(span) for each in-order payload.
    // Returns true if the packet was accounted for (delivered, stashed, or a known duplicate) — caller should ACK.
    // Returns false for beyond-window or oversized; caller must NOT ACK so the sender keeps retransmitting.
    // Spans passed to deliver are valid only for that call's duration.
    template <typename OnProcessPayload>
    bool receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

    // TODO (fragmentation, Phase 3 pending):
    //   1. send() grows a split path: if payload.size() > MAX_FRAGMENT_PAYLOAD, carve into
    //      ceil(size / MAX_FRAGMENT_PAYLOAD) fragments, each with its own sequence and
    //      (fragment_id, fragment_count) header fields. All fragments get retransmit tracking.
    //   2. The deliver callback in receive() must route through a reassembler before firing to the user:
    //        if (fragment_count == 1) user_deliver(payload);
    //        else accumulate into m_reassembly_buf; on last fragment, user_deliver(buf); buf.clear();
    //      Window already guarantees in-order delivery, so fragments of one message come in order.
    //   3. Define MAX_FRAGMENT_PAYLOAD (one MTU minus header). MAX_PAYLOAD becomes the per-fragment
    //      slot size; application-level MAX_MESSAGE_SIZE caps total reassembled size (e.g. 64KB).
    //   4. New error paths: fragment_count mismatch between consecutive fragments → drop + log;
    //      reassembly buffer overflow → drop + log.
    //
    // Scaffolded members for the reassembly path:
    //   static constexpr std::size_t MAX_FRAGMENT_PAYLOAD { MAX_PAYLOAD - PACKET_HEADER_WIRE_SIZE };
    //   static constexpr std::size_t MAX_MESSAGE_SIZE     { 64 * 1024 };
    //   std::vector<std::byte> m_reassembly_buf {};
    //   std::uint16_t m_expected_fragment_count { 0 };

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
    // ~192 KB per channel; zero heap allocs on the receive hot path once constructed.
    static constexpr std::size_t WINDOW_SIZE { 128 };
    static constexpr std::size_t MAX_PAYLOAD { 1500 };  // one UDP MTU

    struct ReorderSlot
    {

        std::uint16_t length { 0 };
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

};

template <typename OnProcessPayload>
bool ReliableOrderedChannel::receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
{

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

    const std::size_t slot_idx = (m_window_base + (sequence - next)) % WINDOW_SIZE;

    // Duplicate: bit already set. Already accounted for, ACK again.
    if (m_received.test(slot_idx))
    {
        return true;
    }

    // Stash into the slot.
    auto& slot = m_slots[slot_idx];
    slot.length = static_cast<std::uint16_t>(payload.size());
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
        deliver(std::span<const std::byte>(front.bytes.data(), front.length));
        m_received.reset(m_window_base);
        m_next_expected_seq = *m_next_expected_seq + 1;
        m_window_base = (m_window_base + 1) % WINDOW_SIZE;
    }

    return true;

}

} // namespace maroof::net

#endif // MAROOF_NET_RELIABLE_ORDERED_CHANNEL_HH
