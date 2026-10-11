#ifndef MAROOF_NET_PACKET_POOL_HH
#define MAROOF_NET_PACKET_POOL_HH

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace maroof::net
{

    class PacketPool;

    // RAII handle to one pool slot. Non-copyable; movable (ownership transfers, source becomes empty).
    // Destructor returns the slot to the pool's free list.
    class PooledBuffer
    {
    public:

        PooledBuffer() noexcept = default;  // empty / sentinel state (post-move or default-init)

        PooledBuffer(const PooledBuffer&)            = delete;
        PooledBuffer& operator=(const PooledBuffer&) = delete;

        PooledBuffer(PooledBuffer&& other) noexcept;
        PooledBuffer& operator=(PooledBuffer&& other) noexcept;
        ~PooledBuffer();

        // Writable span over the first size() bytes of the slot — used by serialize() on send.
        std::span<std::byte>       bytes()       noexcept;
        std::span<const std::byte> bytes() const noexcept;

        std::size_t size()  const noexcept { return m_len; }
        bool        empty() const noexcept { return m_pool == nullptr; }

    private:

        friend class PacketPool;
        PooledBuffer(PacketPool* pool, std::size_t slot_idx, std::size_t len) noexcept
            : m_pool { pool }, m_slot_idx { slot_idx }, m_len { len } {}

        PacketPool* m_pool     { nullptr };
        std::size_t m_slot_idx { 0 };
        std::size_t m_len      { 0 };

    };

    // Fixed-size slot pool. SLOT_SIZE covers one serialized packet (max fragment payload + header).
    // Single-threaded — same contract as ReliableOrderedChannel.
    class PacketPool
    {
    public:

        // PACKET_HEADER_WIRE_SIZE (28) + MAX_FRAGMENT_PAYLOAD (1200). Hard-coded to avoid a
        // circular include with ReliableOrderedChannel.hh; keep in sync if those change.
        static constexpr std::size_t SLOT_SIZE { 1228 };

        explicit PacketPool(std::size_t n_slots);

        PacketPool(const PacketPool&)            = delete;
        PacketPool& operator=(const PacketPool&) = delete;
        PacketPool(PacketPool&&)                 = delete;
        PacketPool& operator=(PacketPool&&)      = delete;

        // Grab a slot sized to `len` bytes.
        // Throws std::length_error if len > SLOT_SIZE (programming error).
        // Throws std::bad_alloc if the pool is exhausted (tune n_slots to match max in-flight).
        PooledBuffer acquire(std::size_t len);

        // Called by PooledBuffer's dtor to put the slot back on the free list.
        void release(std::size_t slot_idx) noexcept;

        std::size_t capacity()     const noexcept { return m_slots.size(); }
        std::size_t slots_in_use() const noexcept { return m_slots.size() - m_free_list.size(); }

        // Used by PooledBuffer::bytes() to form a span over the slot.
        std::byte* slot_data(std::size_t slot_idx) noexcept { return m_slots[slot_idx].data(); }

    private:

        std::vector<std::array<std::byte, SLOT_SIZE>> m_slots;
        std::vector<std::size_t>                      m_free_list;

    };

} // namespace maroof::net

#endif // MAROOF_NET_PACKET_POOL_HH
