# Worker - 工作者服务器
# 部署在被监控机器上，采集性能数据并推送给管理者服务器

# 查找 gRPC
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_library(GRPC_LIBRARY grpc)
    find_library(GRPCPP_LIBRARY grpc++)
    if(GRPC_LIBRARY AND GRPCPP_LIBRARY)
        add_library(grpc_lib INTERFACE)
        target_link_libraries(grpc_lib INTERFACE ${GRPC_LIBRARY} ${GRPCPP_LIBRARY})
        add_library(gRPC::grpc++ ALIAS grpc_lib)
    endif()
endif()

# eBPF 支持选项
option(ENABLE_EBPF "Enable eBPF-based network monitoring" ON)

set(DIAGNOSTICS_SOURCES
    src/diagnostics/observability_config.cpp
    src/diagnostics/anomaly_detector.cpp
    src/diagnostics/observability_state.cpp
    src/diagnostics/remote_health_feedback.cpp
    src/diagnostics/probe_controller.cpp
    src/diagnostics/profile_session.cpp
    src/diagnostics/symbolizer.cpp
)

# 查找 libbpf
if(ENABLE_EBPF)
    find_library(LIBBPF_LIBRARY bpf PATHS /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_library(LIBELF_LIBRARY elf)
    find_library(LIBZ_LIBRARY z)
    
    if(EXISTS "/usr/include/bpf/libbpf.h")
        set(LIBBPF_INCLUDE_DIR "/usr/include")
    else()
        find_path(LIBBPF_INCLUDE_DIR bpf/libbpf.h)
    endif()
    
    if(LIBBPF_LIBRARY AND LIBELF_LIBRARY AND LIBZ_LIBRARY AND LIBBPF_INCLUDE_DIR)
        set(EBPF_FOUND TRUE)
        message(STATUS "eBPF support enabled")
    else()
        message(FATAL_ERROR "ENABLE_EBPF=ON requires libbpf, libelf, libz and headers")
    endif()
endif()

# eBPF skeleton 生成
if(EBPF_FOUND)
    set(EBPF_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ebpf)
    set(EBPF_SKEL_H ${CMAKE_CURRENT_SOURCE_DIR}/include/monitor/net_stats.skel.h)
    set(EBPF_PROGRAM_SRCS
        ${EBPF_SRC_DIR}/net_stats.bpf.c
        ${EBPF_SRC_DIR}/tcp_diag.bpf.c
        ${EBPF_SRC_DIR}/block_io_diag.bpf.c
        ${EBPF_SRC_DIR}/sched_diag.bpf.c
        ${EBPF_SRC_DIR}/oncpu_profile.bpf.c
        ${EBPF_SRC_DIR}/offcpu_profile.bpf.c
    )
    
    add_custom_command(
        OUTPUT ${EBPF_SKEL_H}
        COMMAND make -C ${EBPF_SRC_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${EBPF_SRC_DIR}/.output/net_stats.skel.h ${EBPF_SKEL_H}
        DEPENDS ${EBPF_PROGRAM_SRCS}
        COMMENT "Building eBPF program and generating skeleton"
    )
    add_custom_target(ebpf_skeleton DEPENDS ${EBPF_SKEL_H})
endif()

# 源文件列表
set(WORKER_SOURCES
    src/main.cpp
    src/monitor/metric_collector.cpp
    src/monitor/cpu_load_monitor.cpp
    src/monitor/cpu_softirq_monitor.cpp
    src/monitor/cpu_stat_monitor.cpp
    src/monitor/mem_monitor.cpp
    src/monitor/net_monitor.cpp
    src/monitor/disk_monitor.cpp
    src/monitor/host_info_monitor.cpp
    src/rpc/monitor_pusher.cpp
    src/rpc/monitor_send_queue.cpp
    src/utils/read_file.cpp
    ${DIAGNOSTICS_SOURCES}
)

# 根据 eBPF 支持选择网络监控实现
if(EBPF_FOUND)
    list(APPEND WORKER_SOURCES src/monitor/net_ebpf_monitor.cpp)
endif()

# 可执行文件: worker (工作者服务器)
add_executable(worker ${WORKER_SOURCES})

target_include_directories(worker PUBLIC
    ${PROJECT_SOURCE_DIR}/worker/include
)

# 基础链接库
set(WORKER_LIBS monitor_proto gRPC::grpc++)

# eBPF 依赖
if(EBPF_FOUND)
    add_dependencies(worker ebpf_skeleton)
    target_compile_definitions(worker PRIVATE ENABLE_EBPF=1)
    target_include_directories(worker PRIVATE ${LIBBPF_INCLUDE_DIR})
    list(APPEND WORKER_LIBS ${LIBBPF_LIBRARY} ${LIBELF_LIBRARY} ${LIBZ_LIBRARY})
endif()

target_link_libraries(worker PUBLIC ${WORKER_LIBS} monitor_perf)

if(BUILD_TESTING)
    add_executable(diagnostics_test
        tests/diagnostics_test.cpp
        src/rpc/monitor_send_queue.cpp
        ${DIAGNOSTICS_SOURCES}
    )
    target_include_directories(diagnostics_test PRIVATE
        ${PROJECT_SOURCE_DIR}/worker/include
    )
    target_link_libraries(diagnostics_test PRIVATE monitor_proto monitor_perf)
    add_test(NAME diagnostics_test COMMAND diagnostics_test)

    add_executable(cpu_stat_monitor_test
        tests/cpu_stat_monitor_test.cpp
        src/monitor/cpu_stat_monitor.cpp
    )
    target_include_directories(cpu_stat_monitor_test PRIVATE
        ${PROJECT_SOURCE_DIR}/worker/include
    )
    target_link_libraries(cpu_stat_monitor_test PRIVATE monitor_proto)
    add_test(NAME cpu_stat_monitor_test COMMAND cpu_stat_monitor_test)

    add_executable(observability_overhead_test
        tests/observability_overhead_test.cpp
        src/rpc/monitor_send_queue.cpp
        src/diagnostics/observability_config.cpp
        src/diagnostics/anomaly_detector.cpp
        src/diagnostics/observability_state.cpp
    )
    target_include_directories(observability_overhead_test PRIVATE
        ${PROJECT_SOURCE_DIR}/worker/include
    )
    target_link_libraries(observability_overhead_test PRIVATE monitor_proto)
    add_test(NAME observability_overhead_test COMMAND observability_overhead_test)

    add_executable(collector_contract_test
        tests/collector_contract_test.cpp
        src/monitor/cpu_softirq_monitor.cpp
        src/monitor/cpu_stat_monitor.cpp
        src/monitor/disk_monitor.cpp
        src/monitor/net_monitor.cpp
        src/diagnostics/observability_config.cpp
    )
    target_include_directories(collector_contract_test PRIVATE
        ${PROJECT_SOURCE_DIR}/worker/include
    )
    target_link_libraries(collector_contract_test PRIVATE monitor_proto)
    add_test(NAME collector_contract_test COMMAND collector_contract_test)

    add_executable(perf_log_test tests/perf_log_test.cpp)
    target_link_libraries(perf_log_test PRIVATE monitor_proto monitor_perf)
    add_test(NAME perf_log_test COMMAND perf_log_test)
endif()

# 内核模块构建（可选）
add_custom_target(kernel_modules
    COMMAND make -C ${CMAKE_CURRENT_SOURCE_DIR}/src/kmod
    COMMENT "Building kernel modules"
)

# eBPF 程序构建（可选）
if(EBPF_FOUND)
    add_custom_target(ebpf_programs
        COMMAND make -C ${CMAKE_CURRENT_SOURCE_DIR}/src/ebpf
        COMMENT "Building eBPF programs"
    )
endif()
