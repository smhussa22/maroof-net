#include <gtest/gtest.h>
#include <maroof/net/Endpoint.hh>

#include <arpa/inet.h>
#include <stdexcept>

using maroof::net::Endpoint;

TEST(EndpointTest, DefaultConstructionIsZero)
{
    Endpoint ep;
    EXPECT_EQ(ep.to_string(), "0.0.0.0:0");
}

TEST(EndpointTest, IpPortConstructor)
{
    Endpoint ep("127.0.0.1", 9000);
    EXPECT_EQ(ep.to_string(), "127.0.0.1:9000");
}

TEST(EndpointTest, StringConstructor)
{
    Endpoint ep("192.168.1.42:8080");
    EXPECT_EQ(ep.to_string(), "192.168.1.42:8080");
}

TEST(EndpointTest, StringConstructor_MissingColon_Throws)
{
    EXPECT_THROW(Endpoint("127.0.0.1"), std::invalid_argument);
}

TEST(EndpointTest, StringConstructor_BadPort_Throws)
{
    EXPECT_THROW(Endpoint("127.0.0.1:abc"), std::invalid_argument);
}

TEST(EndpointTest, StringConstructor_PortOutOfRange_Throws)
{
    // 70000 doesn't fit in uint16_t; from_chars should return result_out_of_range.
    EXPECT_THROW(Endpoint("127.0.0.1:70000"), std::invalid_argument);
}

TEST(EndpointTest, StringConstructor_BadIp_Throws)
{
    EXPECT_THROW(Endpoint("not-an-ip:9000"), std::invalid_argument);
}

TEST(EndpointTest, IpPortConstructor_BadIp_Throws)
{
    EXPECT_THROW(Endpoint("300.400.500.600", 9000), std::invalid_argument);
}

TEST(EndpointTest, Setters)
{
    Endpoint ep;
    ep.set_ipv4_address("10.0.0.5");
    ep.set_port_number(8080);
    EXPECT_EQ(ep.to_string(), "10.0.0.5:8080");
}

TEST(EndpointTest, SockaddrInRoundTrip)
{
    Endpoint original("192.168.1.100", 12345);
    sockaddr_in addr = original.to_sockaddr_in();

    EXPECT_EQ(addr.sin_family, AF_INET);
    EXPECT_EQ(ntohs(addr.sin_port), 12345);

    Endpoint restored = Endpoint::from_sockaddr_in(addr);
    EXPECT_TRUE(original == restored);
    EXPECT_EQ(restored.to_string(), "192.168.1.100:12345");
}

TEST(EndpointTest, Equality)
{
    Endpoint a("127.0.0.1", 9000);
    Endpoint b("127.0.0.1", 9000);
    Endpoint c_diff_port("127.0.0.1", 9001);
    Endpoint d_diff_ip  ("127.0.0.2", 9000);

    EXPECT_TRUE (a == b);
    EXPECT_FALSE(a == c_diff_port);
    EXPECT_FALSE(a == d_diff_ip);
}

TEST(EndpointTest, CopyPreservesEquality)
{
    Endpoint original("10.20.30.40", 5555);
    Endpoint copy = original;
    EXPECT_TRUE(original == copy);
    EXPECT_EQ(copy.to_string(), original.to_string());
}
