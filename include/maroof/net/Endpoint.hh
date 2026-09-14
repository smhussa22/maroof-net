#ifndef MAROOF_NET_ENDPOINT_HH
#define MAROOF_NET_ENDPOINT_HH

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <netinet/in.h>

namespace maroof::net
{
    
    class Endpoint
    {
    public:

        Endpoint();                                     // Valid but caller must manually set.
        Endpoint(std::string_view ip, uint16_t port);   // Takes in ip address string and port number separately.
        Endpoint(std::string_view endpoint);            // Takes in an entire string like "127.0.0.1:9000"
        
        // pass in an ipv4 string such as "127.0.0.1".
        void set_ipv4_address(std::string_view ip);

        // pass in a regular port number. does not check for overflow.
        void set_port_number(uint16_t port);

        std::string to_string() const;       // Returns "ip:port", e.g. "127.0.0.1:9000"
        sockaddr_in to_sockaddr_in() const;  // Converts to POSIX struct for use in socket syscalls

        // Static factory; builds an Endpoint from what recvfrom gives you after a packet arrives.
        static Endpoint from_sockaddr_in(const ::sockaddr_in& addr);

        bool operator==(const Endpoint&) const; // Needed for use as a map key.

        // Hash value combining ip and port. Used by std::hash<Endpoint> below,
        // which is what unordered_map<Endpoint, ...> reaches for.
        std::size_t hash_value() const noexcept;

        ~Endpoint() = default;
        
        // The default versions are just fine
        Endpoint(const Endpoint&) = default;
        Endpoint& operator=(const Endpoint&) = default;

        Endpoint(Endpoint&&) = default;
        Endpoint& operator=(Endpoint&&) = default;

    private:
        
        // 32 bit ipv4 address stored in network byte order
        uint32_t m_ipv4_address;
        // 16 bit port number stored in network byte order
        uint16_t m_port_number;

    };

} // namespace maroof::net

// std::hash specialization so Endpoint works as a key in unordered_map/unordered_set
// without callers having to provide a custom hasher. Must live in namespace std
namespace std
{

    template <>
    struct hash<maroof::net::Endpoint>
    {

        size_t operator()(const maroof::net::Endpoint& ep) const noexcept
        {

            return ep.hash_value();

        }

    };

}

#endif // MAROOF_NET_ENDPOINT_HH