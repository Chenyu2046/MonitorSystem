// SPDX-License-Identifier: GPL-2.0
/* Bounded perf-event On-CPU stack sampling. */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define MAX_STACK_DEPTH 64

struct oncpu_stack_key {
    __u32 tgid;
    __u32 pid;
    __s32 user_stack_id;
    __s32 kernel_stack_id;
};

struct oncpu_stack_value {
    __u64 samples;
};

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, __u64[MAX_STACK_DEPTH]);
} oncpu_stack_traces SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, 4096);
    __type(key, struct oncpu_stack_key);
    __type(value, struct oncpu_stack_value);
} oncpu_stack_counts SEC(".maps");

SEC("perf_event")
int oncpu_sample(struct bpf_perf_event_data *ctx) {
    const __u64 pid_tgid = bpf_get_current_pid_tgid();
    struct oncpu_stack_key key = {
        .tgid = pid_tgid >> 32,
        .pid = (__u32)pid_tgid,
        .user_stack_id = bpf_get_stackid(ctx, &oncpu_stack_traces,
                                         BPF_F_USER_STACK),
        .kernel_stack_id = bpf_get_stackid(ctx, &oncpu_stack_traces, 0),
    };
    struct oncpu_stack_value initial = {.samples = 1};
    struct oncpu_stack_value *value =
        bpf_map_lookup_elem(&oncpu_stack_counts, &key);
    if (value) {
        value->samples++;
    } else {
        bpf_map_update_elem(&oncpu_stack_counts, &key, &initial, BPF_ANY);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
