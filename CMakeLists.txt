cmake_minimum_required(VERSION 3.10.2)
project(monitor_system LANGUAGES CXX)

include(CTest)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)

option(BUILD_MANAGER "Build manager server (requires MySQL)" ON)
option(BUILD_BENCHMARK "Build the gRPC benchmark client" ON)
set(SANITIZER "" CACHE STRING "Sanitizer to use for checks: address, thread, or undefined")

# Header-only performance helpers. Keeping this as an INTERFACE target avoids
# adding a runtime library or changing the Worker/Manager ownership model.
add_library(monitor_perf INTERFACE)
target_include_directories(monitor_perf INTERFACE
    ${CMAKE_SOURCE_DIR}/common/include
)

if(SANITIZER)
    if(MSVC)
        message(FATAL_ERROR "SANITIZER is only supported by the GCC/Clang toolchain")
    endif()
    if(NOT SANITIZER STREQUAL "address" AND
       NOT SANITIZER STREQUAL "thread" AND
       NOT SANITIZER STREQUAL "undefined")
        message(FATAL_ERROR "SANITIZER must be address, thread, or undefined")
    endif()
    add_compile_options(-fno-omit-frame-pointer -fsanitize=${SANITIZER})
    add_link_options(-fno-omit-frame-pointer -fsanitize=${SANITIZER})
    if(SANITIZER STREQUAL "thread")
        # GCC TSan in the benchmark image cannot reserve its shadow region
        # for PIE executables.
        add_compile_options(-fno-pie)
        add_link_options(-no-pie)
    endif()
endif()

# Proto 定义（必须先编译）
add_subdirectory(proto)

# Worker - 工作者服务器，部署在被监控机器上，采集性能数据并推送给管理者
add_subdirectory(worker)

# Manager - 管理者服务器，部署在管理端，接收数据、存储、提供查询服务
if(BUILD_MANAGER)
    add_subdirectory(manager)
endif()

if(BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()
