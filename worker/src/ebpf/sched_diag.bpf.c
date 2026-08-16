// SPDX-License-Identifier: GPL-2.0
/* Bounded scheduler switch and wakeup counters. */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct sched_stats {
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
    count_switch(ctx->prev_pid);
    count_switch(ctx->next_pid);
    return 0;
}

SEC("tracepoint/sched/sched_wakeup")
int trace_sched_wakeup(struct trace_event_raw_sched_wakeup_template *ctx) {
    count_wakeup(ctx->pid);
    return 0;
}

SEC("tracepoint/sched/sched_wakeup_new")
int trace_sched_wakeup_new(struct trace_event_raw_sched_wakeup_template *ctx) {
    count_wakeup(ctx->pid);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
