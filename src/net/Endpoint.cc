#include <maroof/net/Endpoint.hh>

#include <arpa/inet.h>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace maroof::net
{

    Endpoint::Endpoint() : m_ipv4_address { 0 } , m_port_number  { 0 } { }

    Endpoint::Endpoint(std::string_view ip, uint16_t port) : m_ipv4_address { 0 }, m_port_number { 0 }
    {

        set_ipv4_address(ip);
        set_port_number(port);

    }

    Endpoint::Endpoint(std::string_view endpoint) : m_ipv4_address { 0 }, m_port_number { 0 }
    {

        const auto colon = endpoint.find(':');
        if (colon == std::string_view::npos)
        {

            throw std::invalid_argument("[Endpoint::Endpoint(std::string_view)] : Missing ':' in endpoint string");

        }

        const std::string_view port_view = endpoint.substr(colon + 1);

        uint16_t port {};
        const auto* first = port_view.data();
        const auto* last  = port_view.data() + port_view.size();
        const auto [ptr, ec] = std::from_chars(first, last, port);
        
        if (ec != std::errc {} || ptr != last)
        {

            throw std::invalid_argument("[Endpoint::Endpoint] : Invalid port in endpoint string");

        }

        set_ipv4_address(endpoint.substr(0, colon));
        set_port_number(port);

    }

    void Endpoint::set_ipv4_address(std::string_view ip)
    {

        std::string ip_str { ip };
        if (::inet_pton(AF_INET, ip_str.c_str(), &m_ipv4_address) != 1)
        {

            throw std::invalid_argument("[Endpoint::set_ipv4_address] : Invalid IPv4 address");

        }
        
    }

    void Endpoint::set_port_number(uint16_t port)
    {

        m_port_number = htons(port);
    
    }

    std::string Endpoint::to_string() const
    {
        
        char buf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &m_ipv4_address, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(::ntohs(m_port_number));
    
    }

    sockaddr_in Endpoint::to_sockaddr_in() const
    {

        ::sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = m_ipv4_address;
        addr.sin_port = m_port_number;
        return addr;

    }

    Endpoint Endpoint::from_sockaddr_in(const ::sockaddr_in& addr)
    {

        Endpoint ep;
        ep.m_ipv4_address = addr.sin_addr.s_addr;
        ep.m_port_number = addr.sin_port;
        return ep;

    }

    bool Endpoint::operator==(const Endpoint& other) const
    {

        return m_ipv4_address == other.m_ipv4_address && m_port_number  == other.m_port_number;

    }

    std::size_t Endpoint::hash_value() const noexcept
    {

        // Combine the two integer fields into one hash. 
        // Follows what boost::hash_combine does: https://gist.github.com/eugene-malashkin/884e225ff57aca1b9cbe
        const std::size_t h1 = std::hash<std::uint32_t> { }(m_ipv4_address);
        const std::size_t h2 = std::hash<std::uint16_t> { }(m_port_number);
        return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));

    }

} // namespace maroof::net
