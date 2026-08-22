#pragma once

/**
 * @file monitor_structs.h
 * @brief 内核模块与 Worker 共享的 mmap 数据布局。
 *
 * 这些 C-compatible 结构必须与 worker/src/kmod 中的定义保持字段顺序、
 * 类型和容量一致。内核模块写入累计 CPU/SoftIRQ/load 值，用户态只读
 * 映射并自行计算 delta、速率和 Protobuf；修改布局会破坏 mmap ABI。
 */

#include <stdint.h>

/*
 * 内核模块与用户空间共享的数据结构定义
 * 这些结构体必须与内核模块中的定义保持一致
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 逐核 SoftIRQ 累计计数；cpu_name 为空表示数组结束。 */
struct softirq_stat {
    char cpu_name[16];      /* CPU 名称，如 "cpu0" */
    uint64_t hi;            /* HI_SOFTIRQ - 高优先级软中断 */
    uint64_t timer;         /* TIMER_SOFTIRQ - 定时器软中断 */
    uint64_t net_tx;        /* NET_TX_SOFTIRQ - 网络发送软中断 */
    uint64_t net_rx;        /* NET_RX_SOFTIRQ - 网络接收软中断 */
    uint64_t block;         /* BLOCK_SOFTIRQ - 块设备软中断 */
    uint64_t irq_poll;      /* IRQ_POLL_SOFTIRQ - 中断轮询软中断 */
    uint64_t tasklet;       /* TASKLET_SOFTIRQ - tasklet 软中断 */
    uint64_t sched;         /* SCHED_SOFTIRQ - 调度软中断 */
    uint64_t hrtimer;       /* HRTIMER_SOFTIRQ - 高精度定时器软中断 */
    uint64_t rcu;           /* RCU_SOFTIRQ - RCU 软中断 */
};

/* 主机 load average 窗口值；不是 CPU busy 百分比。 */
struct cpu_load {
    float load_avg_1;       /* 1 分钟平均负载 */
    float load_avg_3;       /* 5 分钟平均负载 (注：实际是 5 分钟) */
    float load_avg_15;      /* 15 分钟平均负载 */
};

/* 逐核 CPU 累计时间快照；百分比由用户态前后采样计算。 */
struct cpu_stat {
    char cpu_name[16];      /* CPU 名称 */
    uint64_t user;          /* 用户态时间 */
    uint64_t nice;          /* 低优先级用户态时间 */
    uint64_t system;        /* 内核态时间 */
    uint64_t idle;          /* 空闲时间 */
    uint64_t iowait;        /* I/O 等待时间 */
    uint64_t irq;           /* 硬中断时间 */
    uint64_t softirq;       /* 软中断时间 */
    uint64_t steal;         /* 虚拟化偷取时间 */
    uint64_t guest;         /* 虚拟机运行时间 */
    uint64_t guest_nice;    /* 低优先级虚拟机运行时间 */
};

#ifdef __cplusplus
}
#endif
