#pragma once

#include "query_manager.h"

#include "query_api.pb.h"

namespace monitor {

/** @brief 将当前 SoftIRQ events/s 记录写入兼容的查询响应字段。 */
inline void PopulateSoftIrqRateFields(
    const SoftIrqDetailRecord& input,
    ::monitor::proto::SoftIrqDetailRecord* output) {
  if (!output) return;

  // 调用方使用 add_records() 的新消息；旧累计值 tag 保持未填充。
  output->set_hi_per_sec(input.hi);
  output->set_timer_per_sec(input.timer);
  output->set_net_tx_per_sec(input.net_tx);
  output->set_net_rx_per_sec(input.net_rx);
  output->set_block_per_sec(input.block);
  output->set_irq_poll_per_sec(input.irq_poll);
  output->set_tasklet_per_sec(input.tasklet);
  output->set_sched_per_sec(input.sched);
  output->set_hrtimer_per_sec(input.hrtimer);
  output->set_rcu_per_sec(input.rcu);
}

}  // namespace monitor
