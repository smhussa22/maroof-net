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
    src/net/Connection.cc
)
target_include_directories(maroof_net PUBLIC include/)

# ---------- Examples ----------
add_executable(echo_server examples/echo_server.cc)
target_link_libraries(echo_server PRIVATE maroof_net)

add_executable(echo_client examples/echo_client.cc)
target_link_libraries(echo_client PRIVATE maroof_net)

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
