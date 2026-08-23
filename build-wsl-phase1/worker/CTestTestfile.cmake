# CMake generated Testfile for 
# Source directory: /mnt/f/chenyu_project/monitor_system/worker
# Build directory: /mnt/f/chenyu_project/monitor_system/build-wsl-phase1/worker
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(diagnostics_test "/mnt/f/chenyu_project/monitor_system/build-wsl-phase1/worker/diagnostics_test")
set_tests_properties(diagnostics_test PROPERTIES  _BACKTRACE_TRIPLES "/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;123;add_test;/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;0;")
add_test(cpu_stat_monitor_test "/mnt/f/chenyu_project/monitor_system/build-wsl-phase1/worker/cpu_stat_monitor_test")
set_tests_properties(cpu_stat_monitor_test PROPERTIES  _BACKTRACE_TRIPLES "/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;133;add_test;/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;0;")
add_test(observability_overhead_test "/mnt/f/chenyu_project/monitor_system/build-wsl-phase1/worker/observability_overhead_test")
set_tests_properties(observability_overhead_test PROPERTIES  _BACKTRACE_TRIPLES "/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;146;add_test;/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;0;")
add_test(collector_contract_test "/mnt/f/chenyu_project/monitor_system/build-wsl-phase1/worker/collector_contract_test")
set_tests_properties(collector_contract_test PROPERTIES  _BACKTRACE_TRIPLES "/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;160;add_test;/mnt/f/chenyu_project/monitor_system/worker/CMakeLists.txt;0;")
