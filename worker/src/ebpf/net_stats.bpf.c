// SPDX-License-Identifier: GPL-2.0
/**
 * @file net_stats.bpf.c
 * @brief 基于 TC ingress/egress hook 的接口级网络流量统计程序。
 *
 * 程序挂载在 Linux TC/qdisc ingress/egress 路径，对经过 hook 的 skb 做
 * 接口级流量统计。per-CPU map 使用 skb->ifindex 作为 key，用户态负责
 * 把每个 CPU 的 value 聚合，再计算包/字节速率；程序始终返回 TC_ACT_OK，
 * 不改变数据包转发行为。
 *
 * 当前 map 只提供接口级普通监控；TCP retransmission 和任务级诊断由
 * tcp_diag.bpf.c 等独立 Probe 负责。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* TC 返回值 */
#define TC_ACT_OK       0
#define TC_ACT_SHOT     2
#define TC_ACT_UNSPEC   -1

/* 与用户态 NetStats 对齐的接口累计字节/包计数结构。 */
struct net_stats {
    __u64 rcv_bytes;      /* 接收字节数 */
    __u64 rcv_packets;    /* 接收包数 */
    __u64 snd_bytes;      /* 发送字节数 */
    __u64 snd_packets;    /* 发送包数 */
};

/* 
 * Per-CPU BPF Hash Map: key = ifindex (网卡索引), value = net_stats
 * 每个 CPU 维护自己的计数，避免热点网卡在高 PPS 场景竞争同一份 value。
 * 用户空间读取后负责归并所有 CPU 的统计值。
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 64);        /* 最多支持 64 个网卡 */
    __type(key, __u32);             /* ifindex */
    __type(value, struct net_stats);
} net_stats_map SEC(".maps");

/*
 * 更新当前 CPU 上的网卡统计数据。Per-CPU map 为每个 CPU 提供独立
 * value，不需要跨 CPU 原子操作；用户态读取时必须归并所有 CPU。
 */
static __always_inline void update_stats(__u32 ifindex, __u32 len, bool is_rx)
{
    /* ifindex 是 TC skb 绑定的网卡标识，is_rx 决定写入收包还是发包字段。 */
    struct net_stats *stats;
    struct net_stats new_stats = {};

    // 根据当前包的 ifindex，在 net_stats_map 里查找这张网卡已有的统计数据。
    // 如果查到了，就在原有统计上累加；
    // 如果没查到，就说明第一次看到这张网卡，需要新建一条统计记录。 
    stats = bpf_map_lookup_elem(&net_stats_map, &ifindex);
    if (!stats) {
        /* 首次看到此网卡，初始化统计 */
        if (is_rx) {
            new_stats.rcv_bytes = len;
            new_stats.rcv_packets = 1;
        } else {
            new_stats.snd_bytes = len;
            new_stats.snd_packets = 1;
        }
        bpf_map_update_elem(&net_stats_map, &ifindex, &new_stats, BPF_ANY);
    } else {
        /* 更新当前 CPU 的本地计数 */
        if (is_rx) {
            stats->rcv_bytes += len;
            stats->rcv_packets += 1;
        } else {
            stats->snd_bytes += len;
            stats->snd_packets += 1;
        }
    }
}

/*
 * TC Ingress Hook - 入方向流量统计
 * 
 * 当数据包经过 TC ingress hook 时触发；当前实现使用 skb->ifindex
 * 作为入口网卡的 map key。
 */
SEC("tc/ingress")
int tc_ingress(struct __sk_buff *skb)
{
    /* ingress 在数据包进入协议栈时触发，统计后放行原包。 */
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;

    /* 过滤无效数据 */
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    update_stats(ifindex, len, true);

    /* TC_ACT_OK: 继续正常处理，不影响数据包 */
    return TC_ACT_OK;
}

/*
 * TC Egress Hook - 出方向流量统计
 * 
 * 当数据包从协议栈发送到网卡时触发
 */
SEC("tc/egress")
int tc_egress(struct __sk_buff *skb)
{
    /* egress 在数据包离开协议栈时触发，统计后放行原包。 */
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;

    /* 过滤无效数据 */
    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    update_stats(ifindex, len, false);

    /* TC_ACT_OK: 继续正常处理，不影响数据包 */
    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
