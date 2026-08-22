// SPDX-License-Identifier: GPL-2.0
/**
 * @file block_io_diag.bpf.c
 * @brief block_rq_issue/complete 事件之间的块 I/O 延迟聚合程序。
 *
 * issue 阶段按 request 指针保存时间戳、PID 和读写类型，complete 阶段
 * 计算纳秒延迟并写入 per-CPU stats。用户态把各 CPU value 汇总成总次数、
 * 平均延迟和最大延迟；这是诊断证据，不是普通 /proc/diskstats 概览。
 */

#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define REQ_OP_READ 0
#define REQ_OP_WRITE 1

struct block_io_key {
    /* request 指针在 issue/complete 之间标识同一个块请求。 */
    __u64 request;
};

struct block_io_start {
    /* 暂存请求开始时刻和发起线程，供完成事件计算延迟。 */
    __u64 timestamp_ns;
    __u32 pid;
    __u32 op;
};

struct block_io_stats {
    /* per-CPU 累计聚合值，避免高 I/O 事件下跨 CPU 竞争。 */
    __u64 count;
    __u64 read_count;
    __u64 write_count;
    __u64 total_latency_ns;
    __u64 max_latency_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, struct block_io_key);
    __type(value, struct block_io_start);
} block_io_start_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct block_io_stats);
} block_io_stats_map SEC(".maps");

SEC("tp_btf/block_rq_issue")
int BPF_PROG(trace_block_issue, struct request *request) {
    /* block_rq_issue 在请求进入块层时触发，只记录轻量起始状态。 */
    if (!request) {
        return 0;
    }

    struct block_io_key key = {.request = (__u64)request};
    struct block_io_start start = {
        .timestamp_ns = bpf_ktime_get_ns(),
        .pid = (__u32)bpf_get_current_pid_tgid(),
        .op = BPF_CORE_READ(request, cmd_flags) & 0xff,
    };
    bpf_map_update_elem(&block_io_start_map, &key, &start, BPF_ANY);
    return 0;
}

SEC("tp_btf/block_rq_complete")
int BPF_PROG(trace_block_complete, struct request *request,
             blk_status_t error, unsigned int nr_bytes) {
    /* 完成事件闭合 issue 时间窗；error/nr_bytes 当前不参与评分，但保留
     * hook 参数以匹配内核 tracepoint 签名。 */
    (void)error;
    (void)nr_bytes;
    if (!request) {
        return 0;
    }

    struct block_io_key key = {.request = (__u64)request};
    struct block_io_start *start =
        bpf_map_lookup_elem(&block_io_start_map, &key);
    if (!start) {
        return 0;
    }

    const __u64 latency_ns = bpf_ktime_get_ns() - start->timestamp_ns;
    __u32 zero = 0;
    struct block_io_stats *stats =
        bpf_map_lookup_elem(&block_io_stats_map, &zero);
    if (stats) {
        stats->count++;
        stats->total_latency_ns += latency_ns;
        if (latency_ns > stats->max_latency_ns) {
            stats->max_latency_ns = latency_ns;
        }
        if (start->op == REQ_OP_WRITE) {
            stats->write_count++;
        } else {
            stats->read_count++;
        }
    }
    bpf_map_delete_elem(&block_io_start_map, &key);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
