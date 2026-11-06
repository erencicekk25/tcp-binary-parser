cmake_minimum_required(VERSION 3.14)

project(tcp-binary-parser
    VERSION 1.0.0
    DESCRIPTION "Header-only binary packet protocol and lightweight TCP server for IoT devices"
    LANGUAGES CXX
)

option(TBP_BUILD_SERVER "Build the TCP server executable (POSIX only)" ON)
option(TBP_BUILD_TESTS "Build the protocol unit tests" ON)
option(TBP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Warnings ---------------------------------------------------------------

add_library(tbp_warnings INTERFACE)
if(MSVC)
    target_compile_options(tbp_warnings INTERFACE /W4 /permissive-)
    if(TBP_WARNINGS_AS_ERRORS)
        target_compile_options(tbp_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(tbp_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)
    if(TBP_WARNINGS_AS_ERRORS)
        target_compile_options(tbp_warnings INTERFACE -Werror)
    endif()
endif()

# --- Header-only protocol library -------------------------------------------

add_library(packet_protocol INTERFACE)
add_library(tbp::packet_protocol ALIAS packet_protocol)
target_include_directories(packet_protocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(packet_protocol INTERFACE cxx_std_17)

# --- TCP server executable --------------------------------------------------

if(TBP_BUILD_SERVER)
    if(WIN32)
        message(WARNING "TcpServer uses POSIX sockets; skipping the server on Windows. Build under WSL or Linux.")
    else()
        add_executable(tcp_binary_parser
            src/main.cpp
            src/TcpServer.cpp
        )
        target_link_libraries(tcp_binary_parser PRIVATE tbp::packet_protocol tbp_warnings)
        set_target_properties(tcp_binary_parser PROPERTIES CXX_EXTENSIONS OFF)
    endif()
endif()

# --- Tests ------------------------------------------------------------------

if(TBP_BUILD_TESTS)
    enable_testing()
    add_executable(packet_protocol_tests tests/PacketProtocolTests.cpp)
    target_link_libraries(packet_protocol_tests PRIVATE tbp::packet_protocol tbp_warnings)
    set_target_properties(packet_protocol_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME packet_protocol_tests COMMAND packet_protocol_tests)
endif()

# --- Install ----------------------------------------------------------------

include(GNUInstallDirs)
install(FILES include/PacketProtocol.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TARGET tcp_binary_parser)
    install(TARGETS tcp_binary_parser RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
