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

    // Serialize and register a reliable packet; returns wire bytes to send immediately.
    std::span<const std::byte> send(std::span<const std::byte> payload);

    // Advance retransmit timers. Returns spans to resend. Sets dead() on MAX_RETRIES.
    std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now);

    bool dead() const noexcept { return m_dead; }

    void handle_ack(std::uint64_t sequence);

    PacketHeader make_ack(std::uint64_t received_sequence);

    template <typename OnProcessPayload>
    void receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

private:

    static constexpr std::uint32_t MAX_RETRIES { 8 };
    static constexpr std::chrono::milliseconds INITIAL_TIMEOUT { 100 };
    static constexpr std::chrono::milliseconds MAX_TIMEOUT { 1600 };
    static constexpr std::size_t WINDOW_SIZE { 128 };
    static constexpr std::size_t MAX_PAYLOAD { 1500 };

    struct ReorderSlot
    {
        std::uint16_t length { 0 };
        std::array<std::byte, MAX_PAYLOAD> bytes {};
    };

    std::uint64_t m_next_send_seq { 0 };
    bool m_dead { false };
    std::unordered_map<std::uint64_t, PendingPacket> m_pending {};

    std::optional<std::uint64_t> m_next_expected_seq {};
    std::size_t m_window_base { 0 };
    std::bitset<WINDOW_SIZE> m_received {};
    std::array<ReorderSlot, WINDOW_SIZE> m_slots {};

};

template <typename OnProcessPayload>
void ReliableOrderedChannel::receive(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
{

    if (!m_next_expected_seq.has_value())
    {
        m_next_expected_seq = sequence;
    }

    const std::uint64_t next = *m_next_expected_seq;

    if (sequence < next || sequence >= next + WINDOW_SIZE)
    {
        return;
    }

    if (payload.size() > MAX_PAYLOAD)
    {
        return;
    }

    const std::size_t slot_idx = (m_window_base + (sequence - next)) % WINDOW_SIZE;

    if (m_received.test(slot_idx))
    {
        return;
    }

    auto& slot = m_slots[slot_idx];
    slot.length = static_cast<std::uint16_t>(payload.size());
    std::memcpy(slot.bytes.data(), payload.data(), payload.size());
    m_received.set(slot_idx);

    if (sequence != next)
    {
        return;
    }

    while (m_received.test(m_window_base))
    {
        auto& front = m_slots[m_window_base];
        deliver(std::span<const std::byte>(front.bytes.data(), front.length));
        m_received.reset(m_window_base);
        m_next_expected_seq = *m_next_expected_seq + 1;
        m_window_base = (m_window_base + 1) % WINDOW_SIZE;
    }

}

} // namespace maroof::net

#endif // MAROOF_NET_RELIABLE_ORDERED_CHANNEL_HH
