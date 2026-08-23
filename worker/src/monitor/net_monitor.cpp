#include "monitor/net_monitor.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace monitor {

namespace {
bool ParseUnsigned(std::string_view token, std::uint64_t* value) {
  if (!value || token.empty()) return false;
  const auto result = std::from_chars(token.data(), token.data() + token.size(),
                                      *value);
  return result.ec == std::errc() && result.ptr == token.data() + token.size();
}
}  // namespace

bool NetMonitor::Init() {
  std::ifstream file("/proc/net/dev");
  return file.good();
}

bool ParseNetDevLine(const std::string& line, NetRawSample* out) {
  if (!out) return false;
  const auto colon = line.find(':');
  if (colon == std::string::npos) return false;
  NetRawSample sample;
  std::istringstream name_stream(line.substr(0, colon));
  if (!(name_stream >> sample.name)) return false;
  std::string extra_name;
  if (name_stream >> extra_name) return false;

  std::istringstream values(line.substr(colon + 1));
  std::vector<std::uint64_t> fields;
  std::string token;
  while (values >> token) {
    std::uint64_t value = 0;
    if (!ParseUnsigned(token, &value)) return false;
    fields.push_back(value);
  }
  if (fields.size() != 16) return false;
  sample.rcv_bytes = fields[0];
  sample.rcv_packets = fields[1];
  sample.err_in = fields[2];
  sample.drop_in = fields[3];
  sample.snd_bytes = fields[8];
  sample.snd_packets = fields[9];
  sample.err_out = fields[10];
  sample.drop_out = fields[11];
  *out = std::move(sample);
  return true;
}

CollectStatus NetMonitor::UpdateOnce(
    monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) return CollectStatus::kError;
  std::ifstream file("/proc/net/dev");
  if (!file.is_open()) return CollectStatus::kError;
  std::string line;
  if (!std::getline(file, line) || !std::getline(file, line)) {
    return CollectStatus::kError;
  }

  const auto now = std::chrono::steady_clock::now();
  std::vector<NetRawSample> current;
  while (std::getline(file, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    NetRawSample sample;
    if (!ParseNetDevLine(line, &sample)) return CollectStatus::kError;
    if (sample.name == "lo") continue;
    current.push_back(std::move(sample));
  }
  if (file.bad() || current.empty()) return CollectStatus::kError;

  bool not_ready = false;
  struct Report {
    NetRawSample current;
    NetInfo previous;
  };
  std::vector<Report> report;
  report.reserve(current.size());
  for (const auto& sample : current) {
    auto it = last_net_info_.find(sample.name);
    if (it == last_net_info_.end()) {
      not_ready = true;
      continue;
    }
    const auto& previous = it->second.sample;
    if (sample.rcv_bytes < previous.rcv_bytes ||
        sample.rcv_packets < previous.rcv_packets ||
        sample.snd_bytes < previous.snd_bytes ||
        sample.snd_packets < previous.snd_packets) {
      not_ready = true;
    }
    if (std::chrono::duration<double>(now - it->second.timepoint).count() <=
        0) {
      not_ready = true;
    }
    report.push_back({sample, it->second});
  }
  for (const auto& sample : current) {
    last_net_info_[sample.name] = {sample, now};
  }
  if (not_ready) return CollectStatus::kNotReady;

  for (const auto& item : report) {
    const double dt = std::chrono::duration<double>(
                          now - item.previous.timepoint)
                          .count();
    const auto& current_sample = item.current;
    const auto& previous = item.previous.sample;
    auto* net = monitor_info->add_net_info();
    net->set_name(current_sample.name);
    net->set_rcv_rate((current_sample.rcv_bytes - previous.rcv_bytes) /
                      1024.0 / dt);
    net->set_rcv_packets_rate(
        (current_sample.rcv_packets - previous.rcv_packets) / dt);
    net->set_send_rate((current_sample.snd_bytes - previous.snd_bytes) /
                       1024.0 / dt);
    net->set_send_packets_rate(
        (current_sample.snd_packets - previous.snd_packets) / dt);
    net->set_err_in(current_sample.err_in);
    net->set_err_out(current_sample.err_out);
    net->set_drop_in(current_sample.drop_in);
    net->set_drop_out(current_sample.drop_out);
  }
  return CollectStatus::kOk;
}

}  // namespace monitor
