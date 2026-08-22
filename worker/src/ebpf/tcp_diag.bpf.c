// SPDX-License-Identifier: GPL-2.0
/**
 * @file tcp_diag.bpf.c
 * @brief tcp_retransmit_skb tracepoint 下按 TGID/PID 聚合 TCP 重传次数。
 *
 * 该 Probe 只记录发生重传的任务级累计 evidence，用户态按 per-CPU value
 * 汇总后写入 DiagnosticSnapshot；它不负责网络吞吐，也不改变 TCP 行为。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

struct tcp_diag_key {
    /* TGID/PID 让 Manager 能把重传证据关联到进程/线程。 */
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
    /* tracepoint 每发生一次 TCP 重传触发；ctx 当前只用于匹配 hook 签名。 */
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
