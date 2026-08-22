#pragma once

/**
 * @file probe_controller.h
 * @brief Worker 诊断 Probe 的生命周期、重试和快照读取接口。
 *
 * ProbeController 把状态机意图映射为 TCP、Block I/O、Scheduler、OnCPU
 * 和 OffCPU 五类 Probe，负责 ENABLE_EBPF 下的 attach/detach、失败重试、
 * map 快照和 unavailable 状态。普通指标采集不依赖这些 Probe。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "diagnostics/diagnostic_snapshot.h"
#include "diagnostics/observability_state.h"
#include "diagnostics/profile_session.h"

namespace monitor::diagnostics {

/** @brief 诊断探针种类，顺序与内部 status/runtime 数组索引对应。 */
enum class ProbeKind {
  kTcp,
  kBlockIo,
  kScheduler,
  kOnCpuProfile,
  kOffCpuProfile,
};

/**
 * @brief 根据诊断状态控制 eBPF/perf Probe，并读取诊断快照。
 *
 * desired_probes_ 表示策略需要什么，ProbeStatus 表示当前是否请求、可用
 * 和已 attach；两者分离后，ENABLE_EBPF 关闭或 attach 失败时仍能把
 * unavailable 状态传递给上层，而不会伪造诊断数据。
 */
class ProbeController {
 public:
  explicit ProbeController(std::string object_dir = {},
                           int profile_sample_hz = 49,
                           int profile_max_duration_sec = 30);
  ~ProbeController();

  /**
   * @brief 将状态机状态转换为 Probe attach/detach 操作。
   * @return 当前所需 Probe 是否全部可用。
   * @sideeffect 更新状态、重试时间、ProfileSession 和 eBPF runtime。
   */
  bool Apply(
      ObservabilityState state, ProfileType profile_type = ProfileType::kOnCpu,
      ProfileSession::Clock::time_point now = ProfileSession::Clock::now());
  /**
   * @brief 从已 attach Probe 的 BPF map 聚合一轮诊断快照。
   * @return snapshot 非空且读取路径成功时返回 true；未启用 eBPF 时不
   * 产生数据但保持接口可调用。
   */
  bool CollectSnapshot(DiagnosticSnapshot* snapshot) const;

  const std::set<ProbeKind>& DesiredProbes() const { return desired_probes_; }
  std::size_t ApplyCount() const { return apply_count_; }

  /**
   * @brief 单个 Probe 的请求、能力、attach 和退避重试状态。
   *
   * requested 是策略层意图，available/attached 是运行时结果，不能把
   * requested 直接解释成 eBPF 已成功工作。
   */
  struct ProbeStatus {
    bool requested = false;
    bool available = false;
    bool attached = false;
    int last_error = 0;
    std::chrono::steady_clock::time_point next_retry_at =
        std::chrono::steady_clock::time_point::min();
    std::uint32_t retry_count = 0;
  };

  const ProbeStatus& Status(ProbeKind kind) const;

 private:
  struct Runtime;

  static std::size_t Index(ProbeKind kind);
  static std::set<ProbeKind> DesiredFor(ObservabilityState state,
                                        ProfileType profile_type,
                                        bool profile_active);
  void DetachProfile();

  std::string object_dir_;
  int profile_sample_hz_;
  std::chrono::seconds profile_max_duration_;
  std::set<ProbeKind> desired_probes_;
  std::array<ProbeStatus, 5> statuses_{};
  std::size_t apply_count_ = 0;
  bool initialized_ = false;
  std::unique_ptr<Runtime> runtime_;
  std::uint64_t next_profile_id_ = 1;
  std::unique_ptr<ProfileSession> profile_session_;
  bool profile_expired_ = false;
};

}  // namespace monitor::diagnostics
