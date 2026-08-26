// Benchmark-only TC loader. It attaches ingress only and keeps the program alive
// for an externally controlled traffic window.
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

struct net_stats {
  uint64_t rcv_bytes;
  uint64_t rcv_packets;
  uint64_t snd_bytes;
  uint64_t snd_packets;
};

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int) { stop_requested = 1; }

static int seed_map(int fd, int map_type, uint32_t ifindex, int cpus) {
  std::vector<net_stats> values(static_cast<size_t>(cpus));
  const void* data = map_type == BPF_MAP_TYPE_PERCPU_HASH
                         ? static_cast<const void*>(values.data())
                         : static_cast<const void*>(&values[0]);
  return bpf_map_update_elem(fd, &ifindex, data, BPF_ANY);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: map_update_runner OBJECT IFINDEX HOLD_SECONDS\n";
    return 2;
  }

  const std::string object_path = argv[1];
  const int ifindex = std::atoi(argv[2]);
  const int hold_seconds = std::atoi(argv[3]);
  if (ifindex <= 0 || hold_seconds <= 0) return 2;

  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
  bpf_object_open_opts open_opts{};
  open_opts.sz = sizeof(open_opts);
  bpf_object* object = bpf_object__open_file(object_path.c_str(), &open_opts);
  if (!object) {
    std::cerr << "bpf_object__open_file failed\n";
    return 1;
  }
  if (bpf_object__load(object) != 0) {
    std::cerr << "bpf_object__load failed\n";
    bpf_object__close(object);
    return 1;
  }

  bpf_map* map = bpf_object__next_map(object, nullptr);
  bpf_program* ingress = bpf_object__next_program(object, nullptr);
  if (!map || !ingress) {
    std::cerr << "benchmark map/program not found\n";
    bpf_object__close(object);
    return 1;
  }
  const int map_fd = bpf_map__fd(map);
  const int map_type = bpf_map__type(map);
  const int cpus = libbpf_num_possible_cpus();
  if (map_fd < 0 || cpus <= 0 || seed_map(map_fd, map_type,
                                           static_cast<uint32_t>(ifindex), cpus) != 0) {
    std::cerr << "map setup failed: " << std::strerror(errno) << "\n";
    bpf_object__close(object);
    return 1;
  }

  bpf_tc_hook hook{};
  hook.sz = sizeof(hook);
  hook.ifindex = ifindex;
  hook.attach_point = BPF_TC_INGRESS;
  int err = bpf_tc_hook_create(&hook);
  if (err && err != -EEXIST) {
    std::cerr << "bpf_tc_hook_create failed: " << err << "\n";
    bpf_object__close(object);
    return 1;
  }

  bpf_tc_opts opts{};
  opts.sz = sizeof(opts);
  opts.prog_fd = bpf_program__fd(ingress);
  opts.handle = 1;
  opts.priority = 100;
  err = bpf_tc_attach(&hook, &opts);
  if (err) {
    std::cerr << "ingress attach failed: " << err << "\n";
    bpf_object__close(object);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  bpf_prog_info info{};
  __u32 info_len = sizeof(info);
  if (bpf_obj_get_info_by_fd(opts.prog_fd, &info, &info_len) == 0) {
    std::cout << "READY prog_id=" << info.id << " map_type=" << map_type
              << " cpus=" << cpus << "\n" << std::flush;
  } else {
    std::cout << "READY map_type=" << map_type << " cpus=" << cpus << "\n"
              << std::flush;
  }

  for (int i = 0; i < hold_seconds && !stop_requested; ++i)
    std::this_thread::sleep_for(std::chrono::seconds(1));

  bpf_tc_detach(&hook, &opts);
  bpf_object__close(object);
  return 0;
}
