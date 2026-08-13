#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "monitor_info.grpc.pb.h"

namespace {

struct Options {
  std::string target = "manager:50051";
  std::string output = "/results/push-benchmark.csv";
  int workers = 10;
  int duration_seconds = 30;
  int interval_ms = 1000;
  std::string run_id = "benchmark";
  bool stagger_start = false;
};

struct Sample {
  int worker_id;
  int sequence;
  int64_t latency_us;
  bool success;
  int status_code;
};

bool ReadIntArg(const char* value, int* out) {
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (value[consumed] != '\0') return false;
    if (parsed <= 0) return false;
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool IsSafeRunId(const std::string& run_id) {
  if (run_id.empty() || run_id.size() > 64) return false;
  return std::all_of(run_id.begin(), run_id.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
  });
}

bool ParseArgs(int argc, char* argv[], Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--stagger-start") {
      options->stagger_start = true;
      continue;
    }
    if (arg == "--target" || arg == "--output" || arg == "--workers" ||
        arg == "--duration-seconds" || arg == "--interval-ms" ||
        arg == "--run-id") {
      if (++i >= argc) return false;
      if (arg == "--target") options->target = argv[i];
      if (arg == "--output") options->output = argv[i];
      if (arg == "--workers" && !ReadIntArg(argv[i], &options->workers)) return false;
      if (arg == "--duration-seconds" &&
          !ReadIntArg(argv[i], &options->duration_seconds)) return false;
      if (arg == "--interval-ms" && !ReadIntArg(argv[i], &options->interval_ms)) return false;
      if (arg == "--run-id") options->run_id = argv[i];
    } else {
      return false;
    }
  }

  const int64_t expected_samples =
      static_cast<int64_t>(options->workers) * options->duration_seconds * 1000 /
      options->interval_ms;
  return options->workers <= 1000 && options->interval_ms >= 10 &&
         IsSafeRunId(options->run_id) &&
         expected_samples <= 1000000;
}

monitor::proto::MonitorInfo MakeRequest(const std::string& run_id, int worker_id) {
  monitor::proto::MonitorInfo info;
  const std::string name = run_id + "-worker-" + std::to_string(worker_id);
  info.set_name(name);
  info.mutable_host_info()->set_hostname(name);
  info.mutable_host_info()->set_ip_address("10.10.0." + std::to_string(worker_id % 250 + 1));

  auto* cpu = info.add_cpu_stat();
  cpu->set_cpu_name("cpu0");
  cpu->set_cpu_percent(42.0f);
  cpu->set_usr_percent(30.0f);
  cpu->set_system_percent(12.0f);
  info.mutable_cpu_load()->set_load_avg_1(1.0f);
  info.mutable_mem_info()->set_used_percent(55);
  info.mutable_mem_info()->set_total(16384);
  info.mutable_mem_info()->set_free(4096);
  info.mutable_mem_info()->set_avail(8192);
  auto* net = info.add_net_info();
  net->set_name("eth0");
  net->set_send_rate(1024);
  net->set_rcv_rate(2048);
  auto* disk = info.add_disk_info();
  disk->set_name("sda");
  disk->set_util_percent(20.0f);
  return info;
}

int64_t Percentile(std::vector<int64_t> values, double percentile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(std::ceil(values.size() * percentile)) - 1;
  return values[std::min(index, values.size() - 1)];
}

void PrintUsage() {
  std::cerr << "Usage: push_benchmark [--target host:port] [--workers N] "
               "[--duration-seconds N] [--interval-ms N] [--stagger-start] "
               "[--run-id ID] [--output path]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage();
    return 1;
  }

  std::vector<std::vector<Sample>> per_worker(options.workers);
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(options.workers);

  for (int worker_id = 0; worker_id < options.workers; ++worker_id) {
    threads.emplace_back([&, worker_id] {
      auto channel = grpc::CreateChannel(options.target, grpc::InsecureChannelCredentials());
      auto stub = monitor::proto::GrpcManager::NewStub(channel);
      const auto request = MakeRequest(options.run_id, worker_id);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

      const auto worker_start = std::chrono::steady_clock::now() +
          (options.stagger_start
               ? std::chrono::milliseconds(
                     static_cast<int64_t>(worker_id) * options.interval_ms / options.workers)
               : std::chrono::milliseconds(0));
      const auto deadline = worker_start + std::chrono::seconds(options.duration_seconds);
      auto next = worker_start;
      int sequence = 0;
      while (next < deadline) {
        std::this_thread::sleep_until(next);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        google::protobuf::Empty response;
        const auto begin = std::chrono::steady_clock::now();
        const grpc::Status status = stub->SetMonitorInfo(&context, request, &response);
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        per_worker[worker_id].push_back(
            {worker_id, sequence++, latency, status.ok(), status.error_code()});
        next += std::chrono::milliseconds(options.interval_ms);
      }
    });
  }

  const auto benchmark_begin = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  const double elapsed_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - benchmark_begin).count();

  std::ofstream output(options.output);
  if (!output) {
    std::cerr << "Cannot write benchmark output: " << options.output << '\n';
    return 1;
  }
  output << "worker_id,sequence,latency_us,success,status_code\n";
  int total = 0;
  int success = 0;
  std::vector<int64_t> successful_latencies;
  for (const auto& worker_samples : per_worker) {
    for (const auto& sample : worker_samples) {
      output << sample.worker_id << ',' << sample.sequence << ',' << sample.latency_us
             << ',' << (sample.success ? 1 : 0) << ',' << sample.status_code << '\n';
      ++total;
      if (sample.success) {
        ++success;
        successful_latencies.push_back(sample.latency_us);
      }
    }
  }

  std::cout << "samples=" << total << " success=" << success
            << " success_rate=" << (total ? 100.0 * success / total : 0.0)
            << "% reports_per_second=" << (elapsed_seconds > 0 ? total / elapsed_seconds : 0)
            << " p50_us=" << Percentile(successful_latencies, 0.50)
            << " p95_us=" << Percentile(successful_latencies, 0.95)
            << " p99_us=" << Percentile(successful_latencies, 0.99) << '\n';
  return success == total ? 0 : 2;
}
