#include <maroof/net/Crc32.hh>

#include <array>

namespace maroof::net
{

    namespace
    {

        // Precomputed 256-entry lookup table. constexpr means it's built at
        // compile time and stored in .rodata, so there's no runtime init cost.
        constexpr auto make_table() noexcept
        {
            std::array<std::uint32_t, 256> t {};
            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[i] = c;
            }
            return t;
        }

        constexpr auto CRC_TABLE = make_table();

    }

    void Crc32::update(std::span<const std::byte> data) noexcept
    {
        // Copy state into a local so the compiler can keep it in a register
        // instead of reloading from memory each iteration.
        std::uint32_t s = m_state;

        for (std::byte b : data)
        {
            s = CRC_TABLE[(s ^ std::to_integer<std::uint8_t>(b)) & 0xFFu] ^ (s >> 8);
        }

        m_state = s;
    }

} // namespace maroof::net
