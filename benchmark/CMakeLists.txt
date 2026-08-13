add_executable(push_benchmark src/push_benchmark.cpp)

target_link_libraries(push_benchmark PRIVATE monitor_proto)
