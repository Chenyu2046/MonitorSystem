// SPDX-License-Identifier: GPL-2.0
/* Bounded TCP retransmission diagnostics. */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct tcp_diag_key {
    __u32 tgid;
    __u32 pid;
};

struct tcp_diag_value {
    __u64 retransmissions;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, 1024);
    __type(key, struct tcp_diag_key);
    __type(value, struct tcp_diag_value);
} tcp_diag_map SEC(".maps");

SEC("tracepoint/tcp/tcp_retransmit_skb")
int trace_tcp_retransmit(void *ctx) {
    (void)ctx;
    const __u64 pid_tgid = bpf_get_current_pid_tgid();
    const struct tcp_diag_key key = {
        .tgid = pid_tgid >> 32,
        .pid = (__u32)pid_tgid,
    };
    struct tcp_diag_value initial = {.retransmissions = 1};
    struct tcp_diag_value *value = bpf_map_lookup_elem(&tcp_diag_map, &key);
    if (value) {
        value->retransmissions++;
    } else {
        bpf_map_update_elem(&tcp_diag_map, &key, &initial, BPF_ANY);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
