#pragma once

/**
 * @file diagnostic_snapshot.h
 * @brief Worker eBPF/Profiling 诊断快照的用户态中间结构。
 *
 * ProbeController 从 BPF map 聚合到这些 C++ 结构，MonitorPusher 再将其
 * 转换为 DiagnosticSnapshot Protobuf。字段保留 PID/TID、stack id、样本数
 * 和时延等证据语义；profiling samples 是采样次数，不是精确 CPU 百分比。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace monitor::diagnostics {

constexpr std::size_t kMaxProfileStackDepth = 64;

/** @brief 按 TGID/PID 聚合的 TCP 重传证据。 */
struct TcpDiagnosticSample {
  std::uint32_t tgid = 0;
  std::uint32_t pid = 0;
  std::uint64_t retransmissions = 0;
};

/** @brief 主机块 I/O 请求数、读写分类和纳秒延迟聚合。 */
struct BlockIoDiagnosticSample {
  std::uint64_t count = 0;
  std::uint64_t read_count = 0;
  std::uint64_t write_count = 0;
  std::uint64_t total_latency_ns = 0;
  std::uint64_t max_latency_ns = 0;
};

/** @brief 按 PID 聚合的调度切换和唤醒证据。 */
struct SchedulerDiagnosticSample {
  std::uint32_t pid = 0;
  std::uint64_t switches = 0;
  std::uint64_t wakeups = 0;
};

/**
 * @brief On-CPU perf_event 样本和用户/内核 stack 地址。
 *
 * samples 表示窗口内被采样次数，不能直接当作进程 CPU 使用率。
 */
struct OnCpuProfileSample {
  std::uint32_t tgid = 0;
  std::uint32_t pid = 0;
  std::int32_t user_stack_id = -1;
  std::int32_t kernel_stack_id = -1;
  std::uint64_t samples = 0;
  std::vector<std::uint64_t> user_stack;
  std::vector<std::uint64_t> kernel_stack;
};

/** @brief Off-CPU 调度等待时长、次数和内核 stack 地址。 */
struct OffCpuProfileSample {
  std::uint32_t pid = 0;
  std::int32_t kernel_stack_id = -1;
  std::uint64_t total_duration_ns = 0;
  std::uint64_t samples = 0;
  std::vector<std::uint64_t> kernel_stack;
};

/** @brief 两种 profiling Probe 的可用性和样本集合。 */
struct ProfileSnapshot {
  bool on_cpu_available = false;
  bool off_cpu_available = false;
  std::vector<OnCpuProfileSample> on_cpu;
  std::vector<OffCpuProfileSample> off_cpu;
};

/**
 * @brief 一轮诊断 Probe 的统一快照。
 *
 * available 标记能力/attach 结果，vector 只包含读取成功的聚合样本。
 */
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
