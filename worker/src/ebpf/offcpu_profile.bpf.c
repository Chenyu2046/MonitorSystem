// SPDX-License-Identifier: GPL-2.0
/* Bounded sched_switch Off-CPU duration aggregation. */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct offcpu_start_value {
    __u64 timestamp_ns;
    __s32 kernel_stack_id;
};

struct offcpu_key {
    __u32 pid;
    __s32 kernel_stack_id;
};

struct offcpu_value {
    __u64 total_duration_ns;
    __u64 samples;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct offcpu_start_value);
} offcpu_start SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, __u64);
} offcpu_stack_traces SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, 8192);
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_value);
} offcpu_aggregate SEC(".maps");

static __always_inline void record_switch_out(
    struct trace_event_raw_sched_switch *ctx) {
    if (ctx->prev_pid <= 0) {
        return;
    }
    const __u32 pid = (__u32)ctx->prev_pid;
    struct offcpu_start_value start = {
        .timestamp_ns = bpf_ktime_get_ns(),
        .kernel_stack_id = bpf_get_stackid(ctx, &offcpu_stack_traces, 0),
    };
    bpf_map_update_elem(&offcpu_start, &pid, &start, BPF_ANY);
}

static __always_inline void record_switch_in(
    struct trace_event_raw_sched_switch *ctx) {
    if (ctx->next_pid <= 0) {
        return;
    }
    const __u32 pid = (__u32)ctx->next_pid;
    struct offcpu_start_value *start = bpf_map_lookup_elem(&offcpu_start, &pid);
    if (!start) {
        return;
    }

    struct offcpu_key key = {
        .pid = pid,
        .kernel_stack_id = start->kernel_stack_id,
    };
    struct offcpu_value initial = {
        .total_duration_ns = bpf_ktime_get_ns() - start->timestamp_ns,
        .samples = 1,
    };
    struct offcpu_value *value =
        bpf_map_lookup_elem(&offcpu_aggregate, &key);
    if (value) {
        value->total_duration_ns += initial.total_duration_ns;
        value->samples++;
    } else {
        bpf_map_update_elem(&offcpu_aggregate, &key, &initial, BPF_ANY);
    }
    bpf_map_delete_elem(&offcpu_start, &pid);
}

SEC("tracepoint/sched/sched_switch")
int offcpu_sched_switch(struct trace_event_raw_sched_switch *ctx) {
    record_switch_out(ctx);
    record_switch_in(ctx);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
