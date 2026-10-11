cmake_minimum_required(VERSION 3.24)
project(maroof_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- Library ----------
add_library(maroof_net STATIC
    src/net/UdpSocket.cc
    src/net/Endpoint.cc
    src/net/Crc32.cc
    src/net/PacketHeader.cc
    src/net/ReliableOrderedChannel.cc
    src/net/UnreliableUnorderedChannel.cc
    src/net/UnreliableSequencedChannel.cc
    src/net/Connection.cc
    src/net/LatencyHistogram.cc
)
target_include_directories(maroof_net PUBLIC include/)

# ---------- Examples ----------
add_executable(echo_server examples/echo_server.cc)
target_link_libraries(echo_server PRIVATE maroof_net)

add_executable(echo_client examples/echo_client.cc)
target_link_libraries(echo_client PRIVATE maroof_net)

add_executable(flood_client examples/flood_client.cc)
target_link_libraries(flood_client PRIVATE maroof_net)

add_executable(latency_bench examples/latency_bench.cc)
target_link_libraries(latency_bench PRIVATE maroof_net)

# ---------- Tests (Google Test via FetchContent) ----------
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent googletest from overriding parent project options on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(endpoint_test tests/endpoint_test.cc)
target_link_libraries(endpoint_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(endpoint_test)

add_executable(udp_socket_test tests/udp_socket_test.cc)
target_link_libraries(udp_socket_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(udp_socket_test)

add_executable(packet_header_test tests/packet_header_test.cc)
target_link_libraries(packet_header_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(packet_header_test)

add_executable(connection_test tests/connection_test.cc)
target_link_libraries(connection_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(connection_test)

add_executable(unreliable_channels_test tests/unreliable_channels_test.cc)
target_link_libraries(unreliable_channels_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(unreliable_channels_test)

add_executable(reliable_ordered_channel_test tests/reliable_ordered_channel_test.cc)
target_link_libraries(reliable_ordered_channel_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(reliable_ordered_channel_test)

# ---------- HdrHistogram_c (used by LatencyHistogram) ----------
FetchContent_Declare(
    hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.12.0
)
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hdr_histogram)

# PRIVATE: hdr headers only used inside src/net/LatencyHistogram.cc.
# The public header forward-declares struct hdr_histogram, so consumers of
# maroof_net don't need to see hdr/*.h.
target_link_libraries(maroof_net PRIVATE hdr_histogram_static)

add_executable(latency_histogram_test tests/latency_histogram_test.cc)
target_link_libraries(latency_histogram_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(latency_histogram_test)

