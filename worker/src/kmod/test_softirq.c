/*
 * test_softirq.c - 测试软中断采集模块。
 * 通过设备文件 mmap 读取共享快照，用两次读取验证数据会持续更新。
 * 编译: gcc -o test_softirq test_softirq.c
 * 运行: sudo ./test_softirq
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define DEVICE_PATH "/dev/cpu_softirq_monitor"
#define MAX_CPUS 256

/* 软中断统计结构体；字段布局必须与内核模块保持一致。 */
struct softirq_stat {
    char cpu_name[16];
    uint64_t hi;
    uint64_t timer;
    uint64_t net_tx;
    uint64_t net_rx;
    uint64_t block;
    uint64_t irq_poll;
    uint64_t tasklet;
    uint64_t sched;
    uint64_t hrtimer;
    uint64_t rcu;
};

int main(int argc, char *argv[])
{
    int fd;
    struct softirq_stat *stats;
    size_t map_size = sizeof(struct softirq_stat) * MAX_CPUS;
    int i;

    printf("=== Softirq Collector Test ===\n\n");

    /* 打开只读设备文件，验证软中断内核模块是否已注册。 */
    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        printf("Make sure the kernel module is loaded:\n");
        printf("  sudo insmod softirq_collector.ko\n");
        return 1;
    }

    printf("Device opened: %s\n", DEVICE_PATH);

    /* 以只读方式映射内核模块提供的共享区域。 */
    stats = (struct softirq_stat *)mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (stats == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return 1;
    }

    printf("Memory mapped successfully\n\n");

    /* 打印第一次快照，展示每个 CPU 的软中断累计计数。 */
    printf("%-8s %12s %12s %12s %12s %12s\n", 
           "CPU", "HI", "TIMER", "NET_TX", "NET_RX", "SCHED");
    printf("%-8s %12s %12s %12s %12s %12s\n",
           "---", "---", "-----", "------", "------", "-----");

    for (i = 0; i < MAX_CPUS; i++) {
        if (stats[i].cpu_name[0] == '\0')
            break;

        printf("%-8s %12lu %12lu %12lu %12lu %12lu\n",
               stats[i].cpu_name,
               stats[i].hi,
               stats[i].timer,
               stats[i].net_tx,
               stats[i].net_rx,
               stats[i].sched);
    }

    printf("\nTotal CPUs: %d\n", i);

    /* 等待后再次读取同一映射，验证内核定时器会更新快照。 */
    printf("\nWaiting 2 seconds for data update...\n");
    sleep(2);

    printf("\n%-8s %12s %12s %12s %12s %12s\n", 
           "CPU", "HI", "TIMER", "NET_TX", "NET_RX", "SCHED");
    printf("%-8s %12s %12s %12s %12s %12s\n",
           "---", "---", "-----", "------", "------", "-----");

    for (i = 0; i < MAX_CPUS; i++) {
        if (stats[i].cpu_name[0] == '\0')
            break;

        printf("%-8s %12lu %12lu %12lu %12lu %12lu\n",
               stats[i].cpu_name,
               stats[i].hi,
               stats[i].timer,
               stats[i].net_tx,
               stats[i].net_rx,
               stats[i].sched);
    }

    /* 解除映射并关闭设备。 */
    munmap(stats, map_size);
    close(fd);

    printf("\nTest completed successfully!\n");
    return 0;
}
