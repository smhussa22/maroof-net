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
    std::span<const std::byte> send(std::span<const std::byte> payload);

    // Advance retransmit timers. Returns spans to resend; sets dead() on MAX_RETRIES.
    // Caller checks dead() after each call and tears down the Connection if true.
    std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now);

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

private:

    // Retransmit tuning: initial timeout doubles per retry (capped at MAX_TIMEOUT).
    static constexpr std::uint32_t MAX_RETRIES { 8 };
    static constexpr std::chrono::milliseconds INITIAL_TIMEOUT { 100 };
    static constexpr std::chrono::milliseconds MAX_TIMEOUT { 1600 };

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

    // m_next_expected_seq is unset until the first packet arrives (lazy init handles arbitrary start seq).
    // m_window_base is the circular index of the slot for m_next_expected_seq.
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
