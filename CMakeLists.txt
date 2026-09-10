# Project model for the IDE (CLion indexes and debugs through this).
# cpp_engine/Makefile is the command-line build; keep the two in sync.
cmake_minimum_required(VERSION 3.20)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # lets clangd/other editors see the flags

file(GLOB_RECURSE LOB_SOURCES CONFIGURE_DEPENDS cpp_engine/*.cpp)
file(GLOB_RECURSE LOB_HEADERS CONFIGURE_DEPENDS cpp_engine/*.h)

add_library(lob ${LOB_SOURCES} ${LOB_HEADERS})

target_include_directories(lob PUBLIC
        cpp_engine
        cpp_engine/orderbook
        cpp_engine/continuous_auction
        cpp_engine/call_auction
        cpp_engine/call_auction/rules)

if (MSVC)
    target_compile_options(lob PRIVATE /W4)
else ()
    target_compile_options(lob PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
            -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor)
endif ()

# Uncomment once cpp_engine/main.cpp exists:
# add_executable(lob_app cpp_engine/main.cpp)
# target_link_libraries(lob_app PRIVATE lob)
