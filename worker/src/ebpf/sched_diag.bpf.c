// SPDX-License-Identifier: GPL-2.0
/**
 * @file sched_diag.bpf.c
 * @brief scheduler tracepoint 下按 PID 聚合上下文切换和唤醒次数。
 *
 * sched_switch 同时记录 prev/next 任务，sched_wakeup 和 sched_wakeup_new
 * 记录被唤醒任务。LRU_PERCPU_HASH 限制 map 大小并降低热点竞争；用户态
 * 归并每个 CPU value 后只保留 top N 诊断样本。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct sched_stats {
    /* 一个 PID 在当前诊断窗口内的调度事件累计值。 */
    __u64 switches;
    __u64 wakeups;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct sched_stats);
} sched_stats_map SEC(".maps");

static __always_inline void count_switch(__u32 pid) {
    /* 无效 PID 不进入 map，避免内核线程/占位值污染诊断样本。 */
    if ((__s32)pid <= 0) {
        return;
    }
    struct sched_stats initial = {.switches = 1};
    struct sched_stats *stats = bpf_map_lookup_elem(&sched_stats_map, &pid);
    if (stats) {
        stats->switches++;
    } else {
        bpf_map_update_elem(&sched_stats_map, &pid, &initial, BPF_ANY);
    }
}

static __always_inline void count_wakeup(__u32 pid) {
    /* 唤醒事件按被唤醒 PID 归属，而不是按当前执行者归属。 */
    if ((__s32)pid <= 0) {
        return;
    }
    struct sched_stats initial = {.wakeups = 1};
    struct sched_stats *stats = bpf_map_lookup_elem(&sched_stats_map, &pid);
    if (stats) {
        stats->wakeups++;
    } else {
        bpf_map_update_elem(&sched_stats_map, &pid, &initial, BPF_ANY);
    }
}

SEC("tracepoint/sched/sched_switch")
int trace_sched_switch(struct trace_event_raw_sched_switch *ctx) {
    /* 一个切换同时影响离开 CPU 的 prev 和获得 CPU 的 next。 */
    count_switch(ctx->prev_pid);
    count_switch(ctx->next_pid);
    return 0;
}

SEC("tracepoint/sched/sched_wakeup")
int trace_sched_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    /* 普通唤醒 tracepoint 只增加目标任务的 wakeups 计数。 */
    count_wakeup(ctx->pid);
    return 0;
}

SEC("tracepoint/sched/sched_wakeup_new")
int trace_sched_wakeup_new(struct trace_event_raw_sched_wakeup_template *ctx) {
    /* 新任务唤醒与普通唤醒使用相同的 PID 维度，便于统一聚合。 */
    count_wakeup(ctx->pid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
