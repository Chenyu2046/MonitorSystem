/* SPDX-License-Identifier: GPL-2.0 */
/*
 * net_stats.h - eBPF 网络统计共享数据结构。
 *
 * 此头文件定义 eBPF 程序和用户空间程序共同使用的 map value 布局；
 * 字段顺序和宽度属于共享 ABI，修改时必须同步两侧并谨慎处理兼容性。
 */

#ifndef __NET_STATS_H__
#define __NET_STATS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 网络累计统计；每个 Per-CPU value 记录一份网卡方向计数。 */
struct net_stats {
    uint64_t rcv_bytes;      /* 接收字节数 */
    uint64_t rcv_packets;    /* 接收包数 */
    uint64_t snd_bytes;      /* 发送字节数 */
    uint64_t snd_packets;    /* 发送包数 */
};

/* 用户态通过该名称定位 skeleton 中的 map。 */
#define NET_STATS_MAP_NAME "net_stats_map"

/* 预留的网卡数量上限，供相关采集路径使用。 */
#define MAX_NET_DEVICES 64

#ifdef __cplusplus
}
#endif

#endif /* __NET_STATS_H__ */
