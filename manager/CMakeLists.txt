# Manager - 管理者服务器
# 部署在管理端，接收工作者推送的数据、存储到数据库、提供查询服务

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

# MySQL 支持选项
option(ENABLE_MYSQL "Enable MySQL support for data persistence" ON)

set(MANAGER_SOURCES
    src/main.cpp
    src/host_manager.cpp
    src/host_shard_executor.cpp
    src/persistence_worker.cpp
    src/diagnostics/evidence_builder.cpp
    src/diagnostics/root_cause_engine.cpp
    src/diagnostics/incident_store.cpp
    src/diagnostics/diagnostic_persistence.cpp
    src/query_manager.cpp
    src/rpc/grpc_server.cpp
    src/rpc/query_service.cpp
)

# 可执行文件: manager (管理者服务器)
add_executable(manager ${MANAGER_SOURCES})

target_include_directories(manager PUBLIC
    ${PROJECT_SOURCE_DIR}/manager/include
)

# 基础链接库
set(MANAGER_LIBS monitor_proto gRPC::grpc++)

# MySQL 依赖（可选）
if(ENABLE_MYSQL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQL mysqlclient)
    endif()
    
    if(MYSQL_FOUND)
        message(STATUS "MySQL support enabled")
        target_compile_definitions(manager PRIVATE ENABLE_MYSQL=1)
        target_include_directories(manager PUBLIC ${MYSQL_INCLUDE_DIRS})
        list(APPEND MANAGER_LIBS ${MYSQL_LIBRARIES})
    else()
        message(WARNING "MySQL not found, disabling MySQL support")
    endif()
endif()

target_link_libraries(manager PUBLIC ${MANAGER_LIBS})

if(BUILD_TESTING)
    add_executable(manager_diagnostics_test
        tests/diagnostics_test.cpp
        src/diagnostics/evidence_builder.cpp
        src/diagnostics/root_cause_engine.cpp
        src/diagnostics/incident_store.cpp
        src/diagnostics/diagnostic_persistence.cpp
    )
    target_include_directories(manager_diagnostics_test PRIVATE
        ${PROJECT_SOURCE_DIR}/manager/include
    )
    target_link_libraries(manager_diagnostics_test PRIVATE monitor_proto)
    add_test(NAME manager_diagnostics_test COMMAND manager_diagnostics_test)

    add_executable(manager_concurrency_test
        tests/concurrency_test.cpp
        src/host_shard_executor.cpp
        src/persistence_worker.cpp
    )
    target_include_directories(manager_concurrency_test PRIVATE
        ${PROJECT_SOURCE_DIR}/manager/include
    )
    target_link_libraries(manager_concurrency_test PRIVATE monitor_proto)
    add_test(NAME manager_concurrency_test COMMAND manager_concurrency_test)
endif()
