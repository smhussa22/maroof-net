#include <maroof/net/PacketPool.hh>

#include <new>        // std::bad_alloc
#include <stdexcept>  // std::length_error

namespace maroof::net
{

    // ---- PooledBuffer ----------------------------------------------------------------

    PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
        : m_pool { other.m_pool }
        , m_slot_idx { other.m_slot_idx }
        , m_len { other.m_len }
    {
        other.m_pool = nullptr;
    }

    PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept
    {
        if (this != &other)
        {
            if (m_pool) m_pool->release(m_slot_idx);
            m_pool       = other.m_pool;
            m_slot_idx   = other.m_slot_idx;
            m_len        = other.m_len;
            other.m_pool = nullptr;
        }
        return *this;
    }

    PooledBuffer::~PooledBuffer()
    {
        if (m_pool) m_pool->release(m_slot_idx);
    }

    std::span<std::byte> PooledBuffer::bytes() noexcept
    {
        if (!m_pool) return {};
        return { m_pool->slot_data(m_slot_idx), m_len };
    }

    std::span<const std::byte> PooledBuffer::bytes() const noexcept
    {
        if (!m_pool) return {};
        return { m_pool->slot_data(m_slot_idx), m_len };
    }

    // ---- PacketPool ------------------------------------------------------------------

    PacketPool::PacketPool(std::size_t n_slots)
        : m_slots(n_slots)
    {
        // Reserve exactly n_slots so release()'s push_back never has to grow — keeps it
        // allocation-free on the hot path even though vector::push_back isn't marked noexcept.
        m_free_list.reserve(n_slots);

        // Fill the free list in reverse so pop_back serves 0, 1, 2, ... on the first pass —
        // slightly nicer cache locality for the initial burst of sends.
        for (std::size_t i = n_slots; i-- > 0; )
        {
            m_free_list.push_back(i);
        }
    }

    PooledBuffer PacketPool::acquire(std::size_t len)
    {
        if (len > SLOT_SIZE)
        {
            throw std::length_error("PacketPool::acquire: len exceeds SLOT_SIZE");
        }
        if (m_free_list.empty())
        {
            throw std::bad_alloc {};  // pool exhausted — tune n_slots or add back-pressure
        }

        const std::size_t idx = m_free_list.back();
        m_free_list.pop_back();
        return PooledBuffer { this, idx, len };
    }

    void PacketPool::release(std::size_t slot_idx) noexcept
    {
        // Capacity reserved in ctor — no reallocation here.
        m_free_list.push_back(slot_idx);
    }

} // namespace maroof::net
