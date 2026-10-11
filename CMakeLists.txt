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
    src/net/PacketPool.cc
    src/net/EpollReactor.cc
    src/net/IoUringReactor.cc
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

add_executable(echo_server_uring examples/echo_server_uring.cc)
target_link_libraries(echo_server_uring PRIVATE maroof_net)

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

add_executable(packet_pool_test tests/packet_pool_test.cc)
target_link_libraries(packet_pool_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(packet_pool_test)

add_executable(spsc_queue_test tests/spsc_queue_test.cc)
target_link_libraries(spsc_queue_test PRIVATE maroof_net GTest::gtest_main)
# pthreads for the two-thread integration test
find_package(Threads REQUIRED)
target_link_libraries(spsc_queue_test PRIVATE Threads::Threads)
gtest_discover_tests(spsc_queue_test)

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

# ---------- liburing (used by IoUringReactor) ----------
# liburing's build is autoconf-based, so FetchContent (CMake-only) doesn't fit cleanly.
# ExternalProject drives its ./configure + make from inside our build tree.
include(ExternalProject)
set(LIBURING_INSTALL_DIR ${CMAKE_BINARY_DIR}/liburing-install)

ExternalProject_Add(
    liburing_ep
    GIT_REPOSITORY   https://github.com/axboe/liburing.git
    GIT_TAG          liburing-2.5
    PREFIX           ${CMAKE_BINARY_DIR}/liburing
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${LIBURING_INSTALL_DIR}
    BUILD_COMMAND     make -j
    INSTALL_COMMAND   make install
    BUILD_IN_SOURCE   1
    BUILD_BYPRODUCTS  ${LIBURING_INSTALL_DIR}/lib/liburing.a
)

# set_target_properties yells if the include dir doesn't exist at config time (CMake quirk).
file(MAKE_DIRECTORY ${LIBURING_INSTALL_DIR}/include)

add_library(uring STATIC IMPORTED)
set_target_properties(uring PROPERTIES
    IMPORTED_LOCATION             ${LIBURING_INSTALL_DIR}/lib/liburing.a
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INSTALL_DIR}/include
)
add_dependencies(uring liburing_ep)

target_link_libraries(maroof_net PRIVATE uring)

add_executable(latency_histogram_test tests/latency_histogram_test.cc)
target_link_libraries(latency_histogram_test PRIVATE maroof_net GTest::gtest_main)
gtest_discover_tests(latency_histogram_test)

