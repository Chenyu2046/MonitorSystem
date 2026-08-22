// SPDX-License-Identifier: GPL-2.0
/**
 * @file offcpu_profile.bpf.c
 * @brief sched_switch 驱动的 Off-CPU 阻塞时长聚合程序。
 *
 * switch_out 记录被换出线程的时间戳和内核栈，switch_in 用当前时间减去
 * 起始时间并按 PID/stack 聚合。offcpu_start 是临时状态，aggregate 是
 * 用户态读取的总时长/样本数；这些样本描述等待/未运行时间，不是 CPU
 * 使用率。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define MAX_STACK_DEPTH 64

struct offcpu_start_value {
    /* 每个 PID 一份临时换出状态，直到同一线程再次被调度入。 */
    __u64 timestamp_ns;
    __s32 kernel_stack_id;
};

struct offcpu_key {
    /* 按线程和换出时内核栈归并 Off-CPU 时长。 */
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
    __type(value, __u64[MAX_STACK_DEPTH]);
} offcpu_stack_traces SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, 8192);
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_value);
} offcpu_aggregate SEC(".maps");

static __always_inline void record_switch_out(
    struct trace_event_raw_sched_switch *ctx) {
    /* sched_switch 中 prev_pid 是即将停止运行的任务；记录时间起点。 */
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
    /* next_pid 恢复运行时闭合该线程的等待区间并写入 aggregate。 */
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
    /* 同一 tracepoint 同时完成 out/in，形成尽可能完整的调度等待窗口。 */
    record_switch_out(ctx);
    record_switch_in(ctx);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
