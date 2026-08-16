#pragma once

#include <cstdint>
#include <vector>

namespace monitor::diagnostics {

struct TcpDiagnosticSample {
  std::uint32_t tgid = 0;
  std::uint32_t pid = 0;
  std::uint64_t retransmissions = 0;
};

struct BlockIoDiagnosticSample {
  std::uint64_t count = 0;
  std::uint64_t read_count = 0;
  std::uint64_t write_count = 0;
  std::uint64_t total_latency_ns = 0;
  std::uint64_t max_latency_ns = 0;
};

struct SchedulerDiagnosticSample {
  std::uint32_t pid = 0;
  std::uint64_t switches = 0;
  std::uint64_t wakeups = 0;
};

struct OnCpuProfileSample {
  std::uint32_t tgid = 0;
  std::uint32_t pid = 0;
  std::int32_t user_stack_id = -1;
  std::int32_t kernel_stack_id = -1;
  std::uint64_t samples = 0;
};

struct OffCpuProfileSample {
  std::uint32_t pid = 0;
  std::int32_t kernel_stack_id = -1;
  std::uint64_t total_duration_ns = 0;
  std::uint64_t samples = 0;
};

struct ProfileSnapshot {
  bool on_cpu_available = false;
  bool off_cpu_available = false;
  std::vector<OnCpuProfileSample> on_cpu;
  std::vector<OffCpuProfileSample> off_cpu;
};

struct DiagnosticSnapshot {
  bool tcp_available = false;
  bool block_io_available = false;
  bool scheduler_available = false;
  std::vector<TcpDiagnosticSample> tcp;
  std::vector<BlockIoDiagnosticSample> block_io;
  std::vector<SchedulerDiagnosticSample> scheduler;
  ProfileSnapshot profiling;
};

}  // namespace monitor::diagnostics
