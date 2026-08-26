// Benchmark-only Per-CPU Hash variant. Production net_stats.bpf.c is unchanged.
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>

#define TC_ACT_OK 0

struct net_stats {
    __u64 rcv_bytes;
    __u64 rcv_packets;
    __u64 snd_bytes;
    __u64 snd_packets;
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, struct net_stats);
} percpu_update_map SEC(".maps");

SEC("tc/ingress")
int percpu_update_ingress(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    __u32 len = skb->len;
    struct net_stats *stats;

    if (ifindex == 0 || len == 0)
        return TC_ACT_OK;

    stats = bpf_map_lookup_elem(&percpu_update_map, &ifindex);
    if (!stats)
        return TC_ACT_OK;

    stats->rcv_bytes += len;
    stats->rcv_packets += 1;
    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
