# mmap 零拷贝理解与代码锚点

## 1. 结论

当前项目里说的“mmap 零拷贝”，主要指 CPU 状态和软中断统计这两类指标在**内核态到用户态采集器**这一段没有走传统 `read` / `/proc` 文本读取路径，也没有每轮通过 `copy_to_user` 把一份统计数据复制到用户缓冲区。

它的核心做法是：

```text
内核模块分配一块页对齐内存
    -> 内核定时把统计值写入这块内存
    -> 字符设备实现 .mmap 回调
    -> remap_pfn_range 把内核页映射进 worker 进程地址空间
    -> 用户态把 mmap 返回地址转成结构体指针直接读
```

所以这里的“零拷贝”要限定范围：它不是说整条监控链路完全没有拷贝，也不是说 worker 完全没有系统调用。当前用户态采集器每轮仍然会 `open`、`mmap`、`munmap`、`close`；后续填 protobuf、gRPC 发送也会有正常的用户态组织和序列化开销。它优化的是**统计数据从内核共享区进入用户态读取这一步**，避免了传统读接口中的数据搬运。

## 2. 和传统 read / proc 方式的区别

传统方式一般是：

```text
用户态 read /proc/stat
    -> 进入内核
    -> 内核组织文本数据
    -> copy_to_user 拷贝到用户缓冲区
    -> 用户态解析文本
```

本项目 mmap 方式是：

```text
用户态 mmap 字符设备
    -> 内核把共享统计页映射给用户进程
    -> 用户态直接按 struct 数组读取
```

区别在于，用户态后续读 `stats[i]` 时，不是再让内核拷贝一份数据出来，而是在读已经映射到自己虚拟地址空间的共享页。

## 3. CPU 状态统计这条链路

### 3.1 内核模块维护共享内存

CPU 状态统计的设备名是 [worker/src/kmod/cpu_stat_collector.c:31](../../worker/src/kmod/cpu_stat_collector.c#L31)：

```c
#define DEVICE_NAME "cpu_stat_monitor"
```

共享内存指针定义在 [worker/src/kmod/cpu_stat_collector.c:67](../../worker/src/kmod/cpu_stat_collector.c#L67)：

```c
static struct cpu_stat *cpu_stat_data;      /* 共享内存区域 */
```

模块初始化时分配页对齐内存，位置在 [worker/src/kmod/cpu_stat_collector.c:250](../../worker/src/kmod/cpu_stat_collector.c#L250)：

```c
data_size = PAGE_ALIGN(sizeof(struct cpu_stat) * MAX_CPUS);
cpu_stat_data = (struct cpu_stat *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                                    get_order(data_size));
```

这里使用 `PAGE_ALIGN` 和 `__get_free_pages`，是为了后续能按页把这块内存映射给用户态。

### 3.2 内核定时更新统计数据

CPU 统计更新函数在 [worker/src/kmod/cpu_stat_collector.c:124](../../worker/src/kmod/cpu_stat_collector.c#L124)：

```c
static void update_cpu_stats(void)
{
    int cpu;
    int idx = 0;
    struct kernel_cpustat *kcs;

    for_each_possible_cpu(cpu) {
        kcs = &kcpustat_cpu(cpu);
        cpu_stat_data[idx].user = nsec_to_jiffies(kcs->cpustat[CPUTIME_USER]);
        cpu_stat_data[idx].system = nsec_to_jiffies(kcs->cpustat[CPUTIME_SYSTEM]);
        cpu_stat_data[idx].irq = nsec_to_jiffies(kcs->cpustat[CPUTIME_IRQ]);
        cpu_stat_data[idx].softirq = nsec_to_jiffies(kcs->cpustat[CPUTIME_SOFTIRQ]);
    }
}
```

这说明内核模块不是直接给用户态一个 CPU 使用率百分比，而是把内核里的累计时间统计写入 `cpu_stat_data`。用户态后面通过前后两次累计值做差分，计算使用率。

### 3.3 字符设备把 mmap 接到内核回调

字符设备的文件操作表在 [worker/src/kmod/cpu_stat_collector.c:226](../../worker/src/kmod/cpu_stat_collector.c#L226)：

```c
static const struct file_operations cpu_stat_fops = {
    .owner   = THIS_MODULE,
    .open    = cpu_stat_open,
    .release = cpu_stat_release,
    .mmap    = cpu_stat_mmap,
};
```

这里的关键是 `.mmap = cpu_stat_mmap`。用户态对 `/dev/cpu_stat_monitor` 调 `mmap` 时，内核会进入这个回调。

### 3.4 mmap 回调建立页映射

CPU 状态 mmap 回调在 [worker/src/kmod/cpu_stat_collector.c:195](../../worker/src/kmod/cpu_stat_collector.c#L195)：

```c
static int cpu_stat_mmap(struct file *file, struct vm_area_struct *vma)
```

核心映射逻辑在 [worker/src/kmod/cpu_stat_collector.c:212](../../worker/src/kmod/cpu_stat_collector.c#L212)：

```c
pfn = page_to_pfn(virt_to_page(cpu_stat_data));
ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
```

这两行可以这样理解：

- `virt_to_page(cpu_stat_data)`：根据内核虚拟地址找到对应的物理页。
- `page_to_pfn(...)`：拿到物理页帧号。
- `remap_pfn_range(...)`：把这段物理页帧映射到用户进程的虚拟地址区间。

映射完成后，用户态拿到的地址和内核里的 `cpu_stat_data` 指向同一批物理页，所以用户态读结构体字段时，不需要内核再把一份数据拷贝出来。

### 3.5 用户态直接读映射内存

用户态 CPU 采集器打开字符设备的位置在 [worker/src/monitor/cpu_stat_monitor.cpp:12](../../worker/src/monitor/cpu_stat_monitor.cpp#L12)：

```cpp
int fd = open("/dev/cpu_stat_monitor", O_RDONLY);
```

随后调用 `mmap`，位置在 [worker/src/monitor/cpu_stat_monitor.cpp:17](../../worker/src/monitor/cpu_stat_monitor.cpp#L17)：

```cpp
void* addr = mmap(nullptr, stat_size, PROT_READ, MAP_SHARED, fd, 0);
```

再把返回地址转成结构体指针，位置在 [worker/src/monitor/cpu_stat_monitor.cpp:23](../../worker/src/monitor/cpu_stat_monitor.cpp#L23)：

```cpp
struct cpu_stat* stats = static_cast<struct cpu_stat*>(addr);
```

后续读取 `stats[i].user`、`stats[i].system`、`stats[i].idle` 等字段，并计算前后两次差分，位置从 [worker/src/monitor/cpu_stat_monitor.cpp:24](../../worker/src/monitor/cpu_stat_monitor.cpp#L24) 开始。

这就是“像读自己变量一样读内核统计数据”的代码落点。

## 4. 软中断统计这条链路

软中断统计和 CPU 状态统计是同一套设计。

设备名定义在 [worker/src/kmod/softirq_collector.c:26](../../worker/src/kmod/softirq_collector.c#L26)：

```c
#define DEVICE_NAME "cpu_softirq_monitor"
```

共享内存指针定义在 [worker/src/kmod/softirq_collector.c:56](../../worker/src/kmod/softirq_collector.c#L56)：

```c
static struct softirq_stat *softirq_data;   /* 共享内存区域 */
```

初始化时分配页对齐内存，位置在 [worker/src/kmod/softirq_collector.c:189](../../worker/src/kmod/softirq_collector.c#L189)：

```c
data_size = PAGE_ALIGN(sizeof(struct softirq_stat) * MAX_CPUS);
softirq_data = (struct softirq_stat *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                                       get_order(data_size));
```

内核读取软中断累计计数并写入共享区，位置从 [worker/src/kmod/softirq_collector.c:80](../../worker/src/kmod/softirq_collector.c#L80) 开始：

```c
softirq_data[idx].hi      = kstat_softirqs_cpu(HI_SOFTIRQ, cpu);
softirq_data[idx].timer   = kstat_softirqs_cpu(TIMER_SOFTIRQ, cpu);
softirq_data[idx].net_tx  = kstat_softirqs_cpu(NET_TX_SOFTIRQ, cpu);
softirq_data[idx].net_rx  = kstat_softirqs_cpu(NET_RX_SOFTIRQ, cpu);
```

软中断 mmap 回调在 [worker/src/kmod/softirq_collector.c:135](../../worker/src/kmod/softirq_collector.c#L135)，核心映射逻辑在 [worker/src/kmod/softirq_collector.c:152](../../worker/src/kmod/softirq_collector.c#L152)：

```c
pfn = page_to_pfn(virt_to_page(softirq_data));
ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
```

文件操作表把 `.mmap` 绑定到这个回调，位置在 [worker/src/kmod/softirq_collector.c:166](../../worker/src/kmod/softirq_collector.c#L166)：

```c
static const struct file_operations softirq_fops = {
    .owner   = THIS_MODULE,
    .open    = softirq_open,
    .release = softirq_release,
    .mmap    = softirq_mmap,
};
```

用户态软中断采集器打开 `/dev/cpu_softirq_monitor` 的位置在 [worker/src/monitor/cpu_softirq_monitor.cpp:23](../../worker/src/monitor/cpu_softirq_monitor.cpp#L23)，调用 `mmap` 的位置在 [worker/src/monitor/cpu_softirq_monitor.cpp:34](../../worker/src/monitor/cpu_softirq_monitor.cpp#L34)：

```cpp
void* addr = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
```

然后把地址转成结构体数组，位置在 [worker/src/monitor/cpu_softirq_monitor.cpp:40](../../worker/src/monitor/cpu_softirq_monitor.cpp#L40)：

```cpp
struct softirq_stat* stats = static_cast<struct softirq_stat*>(addr);
```

后面遍历 `stats[i]`，计算各类软中断每秒变化率，并填入 protobuf。

## 5. 面试回答版本

如果面试官问“你们这个 mmap 零拷贝怎么理解”，可以这样答：

> 我们这里的零拷贝是限定在内核统计数据到用户态采集器这一步。传统做法是 worker 周期性读 `/proc` 或字符设备 read，每次都要系统调用、内核组织数据，再 copy 到用户缓冲区。我们这边 CPU 状态和软中断用了内核模块，模块分配页对齐的共享内存保存统计值，并在字符设备的 mmap 回调里用 `remap_pfn_range` 把这批页映射到 worker 进程。用户态拿到地址后转成 `struct cpu_stat*` 或 `struct softirq_stat*` 直接遍历，所以数据读取这一步没有每轮 `copy_to_user`。不过我不会把整条链路说成完全零拷贝，因为当前实现每轮还是会 open/mmap/munmap/close，后续 protobuf 组包和 gRPC 发送也会有序列化开销。

## 6. 当前实现的边界

当前代码里需要注意几个边界：

- 当前 mmap 只覆盖 CPU 状态和软中断统计，不能泛化成所有指标都走 mmap。
- 用户态每次 `UpdateOnce` 都重新 `open`、`mmap`、`munmap`、`close`，所以它不是“采样期间完全无系统调用”。
- mmap 优化的是内核到用户态的数据读取方式，不等于后续 protobuf 和 gRPC 网络发送也零拷贝。
- 当前映射是 `PROT_READ | MAP_SHARED`，用户态按只读方式消费内核统计页。
- 如果继续优化，可以考虑让采集器生命周期内持有 fd 和映射地址，避免每轮重复 mmap；但这属于后续优化，不是当前代码已经实现的能力。

