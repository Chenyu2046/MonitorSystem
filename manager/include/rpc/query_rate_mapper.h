#pragma once

#include "query_api.pb.h"
#include "query_manager.h"

namespace monitor {

inline void PopulatePerformanceRateFields(
    const PerformanceRecord& input,
    ::monitor::proto::PerformanceRecord* output) {
  if (!output) return;
  output->set_cpu_percent_rate(input.cpu_percent_rate);
  output->set_usr_percent_rate(input.usr_percent_rate);
  output->set_system_percent_rate(input.system_percent_rate);
  output->set_io_wait_percent_rate(input.io_wait_percent_rate);
  output->set_load_avg_1_rate(input.load_avg_1_rate);
  output->set_load_avg_3_rate(input.load_avg_3_rate);
  output->set_load_avg_15_rate(input.load_avg_15_rate);
  output->set_mem_used_percent_rate(input.mem_used_percent_rate);
  output->set_disk_util_percent_rate(input.disk_util_percent_rate);
  output->set_send_rate_rate(input.send_rate_rate);
  output->set_rcv_rate_rate(input.rcv_rate_rate);
}

inline void PopulateNetRateFields(
    const NetDetailRecord& input,
    ::monitor::proto::NetDetailRecord* output) {
  if (!output) return;
  output->set_rcv_bytes_rate_rate(input.rcv_bytes_rate_rate);
  output->set_snd_bytes_rate_rate(input.snd_bytes_rate_rate);
  output->set_err_in_rate(input.err_in_rate);
  output->set_err_out_rate(input.err_out_rate);
  output->set_drop_in_rate(input.drop_in_rate);
  output->set_drop_out_rate(input.drop_out_rate);
}

inline void PopulateDiskRateFields(
    const DiskDetailRecord& input,
    ::monitor::proto::DiskDetailRecord* output) {
  if (!output) return;
  output->set_read_bytes_per_sec_rate(input.read_bytes_per_sec_rate);
  output->set_write_bytes_per_sec_rate(input.write_bytes_per_sec_rate);
  output->set_read_iops_rate(input.read_iops_rate);
  output->set_write_iops_rate(input.write_iops_rate);
  output->set_util_percent_rate(input.util_percent_rate);
}

inline void PopulateMemRateFields(
    const MemDetailRecord& input,
    ::monitor::proto::MemDetailRecord* output) {
  if (!output) return;
  output->set_total_rate(input.total_rate);
  output->set_free_rate(input.free_rate);
  output->set_avail_rate(input.avail_rate);
  output->set_active_rate(input.active_rate);
  output->set_inactive_rate(input.inactive_rate);
}

}  // namespace monitor
