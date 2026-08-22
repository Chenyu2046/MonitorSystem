// SPDX-License-Identifier: GPL-2.0
/**
 * @file oncpu_profile.bpf.c
 * @brief perf_event 周期采样的 On-CPU 任务与调用栈聚合程序。
 *
 * perf_event 在当前占用 CPU 的任务上下文中触发，使用
 * bpf_get_current_pid_tgid() 获取 TGID/PID，并通过 stack trace map 保存
 * 用户/内核栈地址。LRU_PERCPU_HASH 只累计采样次数；采样次数是抽样
 * 证据，不等同于精确的进程 CPU 百分比。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define MAX_STACK_DEPTH 64

struct oncpu_stack_key {
    /* TGID 表示进程，PID 表示线程；stack id 作为调用路径维度。 */
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

/* 每次 perf_event 采样增加当前任务和调用栈组合的 samples。 */
SEC("perf_event")
int oncpu_sample(struct bpf_perf_event_data *ctx) {
    /* 当前上下文就是被采样任务；用户态负责把 stack id 再读回地址。 */
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
