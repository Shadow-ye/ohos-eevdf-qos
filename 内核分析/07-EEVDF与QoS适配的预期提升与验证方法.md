# EEVDF+QoS 适配的预期提升与验证方法

> 日期:2026-08-21
> 前置文档:05-EEVDF移植记录.md、06-EEVDF与QoS适配记录.md
> 目的:明确这次改动可能带来哪些提升,以及如何用数据证明

---

## 一、预期提升

### 1.1 调度延迟可预测性

**CFS 的问题:**

CFS 只有一个维度 -- vruntime。任务的调度延迟取决于当前队列中所有任务的 vruntime 分布,这个分布是动态变化的,无法预测。

```
CFS 调度延迟 = f(当前队列中所有任务的 vruntime 分布)
-> 不可预测,取决于运行时状态
```

**EEVDF 的改善:**

EEVDF 引入 virtual deadline。每个任务有明确的截止时间,调度器选 deadline 最早的。deadline 在任务入队时就确定了(由 slice 和 weight 决定),不随运行时间变化。

```
EEVDF 调度延迟 = f(slice, weight)
-> 入队时确定,可预测
```

**实际意义:**

音频渲染线程需要在 20ms 内完成一帧。CFS 下无法保证调度延迟;EEVDF 下可以通过设置合适的 slice 来控制 deadline,使调度延迟满足约束。

### 1.2 QoS 等级区分度增强

**适配前(只有 nice):**

```
USER_INTERACTIVE(nice=-10) 的权重: 约 10240
BACKGROUND(nice=10) 的权重: 约 1024
权重比: 约 10:1

效果: 高 QoS 任务的 vruntime 增速慢 10 倍
局限: 只在一个维度(权重)上有差异
```

**适配后(nice + slice/deadline):**

```
USER_INTERACTIVE: nice=-10, slice=1ms   -> 权重高 + deadline 早
BACKGROUND:       nice=10,  slice=14ms  -> 权重低 + deadline 晚

两个维度叠加:
  维度1: 权重差 10 倍(vruntime 增速差异)
  维度2: deadline 差 14 倍(调度顺序差异)

效果: 高 QoS 任务获得双重优势
```

**直觉理解:**

想象一个排队系统:
- CFS: 所有人按"已经等待了多久"排队,等待最久的先服务
- EEVDF: 所有人按"截止时间"排队,截止时间最早的先服务
- EEVDF+QoS: 高优先级的人获得更早的截止时间 + 更慢的等待计时器

### 1.3 截止时间敏感任务受益

QOS_DEADLINE_REQUEST 等级的 slice=1.5ms,deadline 非常早。

**受益场景:**

| 场景 | 为什么受益 |
|---|---|
| 音频渲染 | 每帧有严格时间约束(如 20ms),deadline 早确保音频线程优先获得 CPU |
| 传感器采样 | 固定频率采样(如 100Hz),deadline 早确保采样间隔稳定 |
| 触摸响应 | 用户点击后需要快速响应,deadline 早减少响应延迟 |
| 动画渲染 | 帧间隔固定(如 16.6ms@60fps),deadline 早减少掉帧 |

### 1.4 后台干扰抑制

BACKGROUND 任务 slice=14ms,deadline 很晚。

**效果:**

```
即使后台有 10 个任务在竞争:
  它们的 deadline 都排在 14ms 之后
  前台任务的 deadline 排在 1-2ms 之前
  -> 前台任务几乎不受后台干扰
```

**CFS 下的问题:**

```
10 个后台任务(nice=10) + 1 个前台任务(nice=-10)
前台任务的 vruntime 增速确实慢 10 倍
但如果后台任务数量多,累积的 vruntime 增量仍然会抢占前台
```

**EEVDF+QoS 下的改善:**

```
10 个后台任务(deadline=14ms) + 1 个前台任务(deadline=1ms)
调度器总是选 deadline 最早的
-> 前台任务优先级不受后台数量影响
```

### 1.5 公平性改善

CFS 的 vruntime 补偿机制有"突变"问题:

```
任务睡眠 1 秒后唤醒:
  CFS:   vruntime 被大幅调整(补偿),可能导致抢占正在运行的任务
  EEVDF: 用 vlag(虚拟滞后)补偿,更平滑,减少突变
```

EEVDF 的 eligible 判定确保:
- 刚唤醒的任务不会立即抢占其他任务(需要先 eligible)
- 但也不会被无限延后(有 deadline 保障)

---

## 二、量化指标

### 2.1 可测量的指标表

| 指标 | 定义 | 预期变化 | 重要性 |
|---|---|---|---|
| 平均调度延迟 | 任务从就绪到运行的平均时间 | USER_INTERACTIVE 降低 30-50% | 高 |
| P99 调度延迟 | 99% 分位的调度延迟 | 降低 40-60% | 高 |
| 最大调度延迟 | 最长的一次调度延迟 | 降低 | 中 |
| 帧率稳定性 | 帧间隔的标准差 | 降低(更稳定) | 高 |
| 后台对前台干扰 | 前后台并发时前台的帧率下降 | 降低 50%+ | 高 |
| 调度切换次数 | 每秒上下文切换次数 | 可能增加(更频繁调度) | 低 |
| CPU 利用率 | CPU 忙碌时间占比 | 基本不变 | 低 |

### 2.2 预期变化幅度

**乐观预期(如果 slice 映射值合适):**

```
USER_INTERACTIVE 平均延迟:  从 ~500us 降到 ~250us
USER_INTERACTIVE P99 延迟:  从 ~2ms   降到 ~800us
BACKGROUND 平均延迟:        从 ~500us 升到 ~1ms
前后台并发帧率下降:         从 15%    降到 5%
```

**保守预期(如果 slice 映射值需要调优):**

```
USER_INTERACTIVE 平均延迟:  从 ~500us 降到 ~350us
USER_INTERACTIVE P99 延迟:  从 ~2ms   降到 ~1.2ms
前后台并发帧率下降:         从 15%    降到 10%
```

---

## 三、验证方法

### 3.1 cyclictest 延迟对比

**目的:** 对比 CFS vs EEVDF+QoS 在不同优先级下的调度延迟分布

**前置条件:** cyclictest 需要在实机(rk3568)上运行,QEMU 不适合做延迟测量

**安装 cyclictest:**

```bash
# 方法1: 板子上有 apt
sudo apt install rt-tests

# 方法2: 交叉编译
git clone https://github.com/clrkwng/rt-tests.git
cd rt-tests
make CROSS_COMPILE=aarch64-linux-gnu-
```

**测试1: 基线测量(CFS 内核)**

```bash
# 先刷回原始 CFS 内核(未修改的),然后运行:

# 1.1 高优先级 FIFO 任务
sudo cyclictest -t1 -p 99 -i 1000 -l 100000 -m --policy=fifo \
    -q -o /tmp/cfs_high_prio.log

# 1.2 普通 SCHED_NORMAL 任务
sudo cyclictest -t1 -p 0 -i 1000 -l 100000 -m \
    -q -o /tmp/cfs_normal.log

# 1.3 低优先级 SCHED_NORMAL 任务
sudo nice -n 19 cyclictest -t1 -p 0 -i 1000 -l 100000 -m \
    -q -o /tmp/cfs_low_prio.log
```

**测试2: EEVDF+QoS 内核测量**

```bash
# 刷入 EEVDF+QoS 内核,然后运行同样的测试:

sudo cyclictest -t1 -p 99 -i 1000 -l 100000 -m --policy=fifo \
    -q -o /tmp/eevdf_high_prio.log

sudo cyclictest -t1 -p 0 -i 1000 -l 100000 -m \
    -q -o /tmp/eevdf_normal.log

sudo nice -n 19 cyclictest -t1 -p 0 -i 1000 -l 100000 -m \
    -q -o /tmp/eevdf_low_prio.log
```

**参数解释:**

| 参数 | 含义 |
|---|---|
| `-t1` | 1 个测试线程 |
| `-p 99` | 最高优先级(99) |
| `-i 1000` | 每 1000us(1ms)唤醒一次 |
| `-l 100000` | 运行 100000 次 |
| `-m` | 使用 mlockall 锁定内存,避免缺页干扰 |
| `--policy=fifo` | 使用 SCHED_FIFO 实时调度 |
| `-q` | 安静模式,只输出统计 |
| `-o <file>` | 输出到文件 |

**结果对比表(预期):**

```
| 测试场景        | CFS Avg | CFS P99 | EEVDF+QoS Avg | EEVDF+QoS P99 | 变化  |
|-----------------|---------|---------|----------------|----------------|-------|
| 高优先级 FIFO   | ~5us    | ~20us   | ~3us           | ~10us          | -40%  |
| 普通 NORMAL     | ~50us   | ~200us  | ~30us          | ~100us         | -50%  |
| 低优先级 NORMAL | ~500us  | ~2ms    | ~800us         | ~3ms           | +60%  |
```

低优先级延迟增加是正常的 -- 它们的 deadline 被推后了,把 CPU 时间让给高优先级任务。

### 3.2 QoS 等级切换验证

**目的:** 验证不同 QoS 等级是否产生不同的调度延迟

**测试程序(test_qos_latency.c):**

```c
/*
 * test_qos_latency.c
 * 测量不同 QoS 等级下的调度延迟
 *
 * 编译(aarch64 交叉编译):
 *   aarch64-linux-gnu-gcc -O2 -o test_qos_latency test_qos_latency.c -lpthread
 *
 * 运行:
 *   adb push test_qos_latency /data/
 *   adb shell "chmod +x /data/test_qos_latency && /data/test_qos_latency"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

/* QoS 等级定义(与 OHOS 一致) */
#define QOS_BACKGROUND       0
#define QOS_UTILITY          1
#define QOS_DEFAULT          2
#define QOS_USER_INITIATED   3
#define QOS_DEADLINE_REQUEST 4
#define QOS_USER_INTERACTIVE 5
#define QOS_KEY_BACKGROUND   6

#define TEST_ITERATIONS 100000

static int cmp_long(const void *a, const void *b) {
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

void measure_latency(int qos_level, const char *name) {
    long latencies[TEST_ITERATIONS];
    struct timespec start, end;

    struct sched_param param = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_NORMAL, &param);

    /* 通过 /proc/thread-self/sched_qos_ctrl 设置 QoS */
    char qos_cmd[128];
    snprintf(qos_cmd, sizeof(qos_cmd),
             "echo '%d' > /proc/thread-self/sched_qos_ctrl 2>/dev/null", qos_level);
    system(qos_cmd);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        sched_yield();
        clock_gettime(CLOCK_MONOTONIC, &end);

        latencies[i] = (end.tv_sec - start.tv_sec) * 1000000000L +
                        (end.tv_nsec - start.tv_nsec);
    }

    qsort(latencies, TEST_ITERATIONS, sizeof(long), cmp_long);

    long sum = 0;
    for (int i = 0; i < TEST_ITERATIONS; i++)
        sum += latencies[i];

    printf("[%s] Avg=%ldns P50=%ldns P99=%ldns P99.9=%ldns Max=%ldns\n",
           name, sum / TEST_ITERATIONS,
           latencies[TEST_ITERATIONS / 2],
           latencies[TEST_ITERATIONS * 99 / 100],
           latencies[TEST_ITERATIONS * 999 / 1000],
           latencies[TEST_ITERATIONS - 1]);
}

int main(void) {
    printf("=== QoS Level Latency Test (%d iterations) ===\n\n", TEST_ITERATIONS);

    measure_latency(QOS_USER_INTERACTIVE, "USER_INTERACTIVE(5)");
    measure_latency(QOS_DEADLINE_REQUEST, "DEADLINE_REQUEST(4)");
    measure_latency(QOS_USER_INITIATED,   "USER_INITIATED(3)");
    measure_latency(QOS_DEFAULT,          "DEFAULT(2)");
    measure_latency(QOS_UTILITY,          "UTILITY(1)");
    measure_latency(QOS_BACKGROUND,       "BACKGROUND(0)");

    printf("\n=== Done ===\n");
    return 0;
}
```

**预期结果:**

```
=== QoS Level Latency Test (100000 iterations) ===

[USER_INTERACTIVE(5)] Avg=180ns P50=150ns P99=400ns P99.9=800ns Max=2000ns
[DEADLINE_REQUEST(4)] Avg=200ns P50=170ns P99=450ns P99.9=900ns Max=2200ns
[USER_INITIATED(3)]   Avg=250ns P50=200ns P99=600ns P99.9=1200ns Max=3000ns
[DEFAULT(2)]          Avg=400ns P50=350ns P99=900ns P99.9=2000ns Max=5000ns
[UTILITY(1)]          Avg=600ns P50=500ns P99=1500ns P99.9=3000ns Max=8000ns
[BACKGROUND(0)]       Avg=900ns P50=750ns P99=2500ns P99.9=5000ns Max=12000ns
```

### 3.3 前后台并发测试

**目的:** 验证后台任务对前台任务的干扰是否降低

**步骤:**

```bash
# 步骤1: 无后台负载(基线)
sudo cyclictest -t1 -p 80 -i 16600 -l 60000 -m -q -o /tmp/fg_only.log
# 16600us = 60fps 帧间隔

# 步骤2: 有后台负载(4个低优先级任务)
# 终端1: 前台
sudo cyclictest -t1 -p 80 -i 16600 -l 60000 -m -q -o /tmp/fg_with_bg.log &

# 终端2: 后台(模拟下载/CPU 密集)
sudo nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
sudo nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
sudo nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
sudo nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &

# 等待前台测试完成,对比两个 log
```

**结果对比表(预期):**

```
| 场景                  | Avg    | P99    | Max    | 说明             |
|-----------------------|--------|--------|--------|------------------|
| 无后台(CFS)           | 5us    | 15us   | 50us   | 基线             |
| 无后台(EEVDF+QoS)     | 4us    | 12us   | 40us   | 略有改善         |
| 有后台(CFS)           | 20us   | 80us   | 300us  | 干扰严重         |
| 有后台(EEVDF+QoS)     | 6us    | 20us   | 80us   | 干扰显著降低     |
```

### 3.4 perf sched 分析

**目的:** 从调度器内部视角分析调度行为变化

**步骤:**

```bash
# 录制调度事件
sudo perf sched record -a -- sleep 5

# 输出调度统计(按最大延迟排序)
sudo perf sched latency --sort max

# 输出调度时序(可视化哪个任务在哪个 CPU 上运行)
sudo perf sched map

# 输出调用链(每个调度切换的详细信息)
sudo perf sched timehist
```

**关注指标:**

```
perf sched latency 输出:
  Task          Runtime  Switches  Average sw  Maximum sw  Average lat  Maximum lat
  <task_name>   1.2s     5000      0.24ms      2.1ms       0.05ms      0.8ms

对比 CFS vs EEVDF+QoS:
  - Maximum sw(最大切换时间): 应该降低
  - Maximum lat(最大延迟):    应该降低
  - Average lat(平均延迟):    应该降低
```

### 3.5 帧率稳定性测试

**目的:** 验证 EEVDF+QoS 对 UI 流畅度的影响

**方法(使用 OHOS fpsgo):**

```bash
hdc shell
fpsgoctrl -t           # 开始追踪
# 运行应用一段时间
fpsgoctrl -f           # 停止追踪

# 查看结果
cat /sys/kernel/debug/fpsgo/fstb/fstb_info
```

**关注指标:**

```
帧率稳定性 = 1 - (帧间隔标准差 / 平均帧间隔)

CFS:        稳定性 ~0.85 (有波动)
EEVDF+QoS:  稳定性 ~0.95 (更平滑)
```

---

## 四、验证环境搭建

### 4.1 rk3568 编译命令

```bash
cd ~/OHOS501

# 编译 rk3568 镜像
echo '3' | sudo -S ./build.sh --product-name rk3568 --ccache

# 编译产物路径
ls out/rk3568/packages/phone/images/
```

### 4.2 刷机方法

```bash
# 连接 rk3568 开发板
# 使用 rkdevtool 刷入以下文件:
#   - boot.img    (内核)
#   - system.img
#   - vendor.img
#   - userdata.img

# 或者只刷内核(快速迭代):
adb push out/rk3568/packages/phone/images/boot.img /sdcard/
adb reboot bootloader
# 通过 fastboot 刷入
```

### 4.3 完整测试脚本

```bash
#!/bin/bash
# run_benchmark.sh - 完整基准测试脚本
# 用法: sudo ./run_benchmark.sh

RESULTS_DIR="/data/benchmark_results/$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULTS_DIR

echo "=== EEVDF+QoS Benchmark ===" | tee $RESULTS_DIR/summary.txt
echo "Date: $(date)" | tee -a $RESULTS_DIR/summary.txt
echo "Kernel: $(uname -r)" | tee -a $RESULTS_DIR/summary.txt
echo "" | tee -a $RESULTS_DIR/summary.txt

# 测试1: cyclictest 高优先级
echo "[Test 1] cyclictest high priority..." | tee -a $RESULTS_DIR/summary.txt
cyclictest -t1 -p 99 -i 1000 -l 100000 -m --policy=fifo \
    -q -o $RESULTS_DIR/cyclictest_high.log
tail -1 $RESULTS_DIR/cyclictest_high.log | tee -a $RESULTS_DIR/summary.txt

# 测试2: cyclictest 普通优先级
echo "[Test 2] cyclictest normal priority..." | tee -a $RESULTS_DIR/summary.txt
cyclictest -t1 -p 0 -i 1000 -l 100000 -m \
    -q -o $RESULTS_DIR/cyclictest_normal.log
tail -1 $RESULTS_DIR/cyclictest_normal.log | tee -a $RESULTS_DIR/summary.txt

# 测试3: QoS 等级延迟
echo "[Test 3] QoS level latency..." | tee -a $RESULTS_DIR/summary.txt
./test_qos_latency 2>&1 | tee -a $RESULTS_DIR/summary.txt

# 测试4: 前后台并发
echo "[Test 4] fg/bg concurrency..." | tee -a $RESULTS_DIR/summary.txt
cyclictest -t1 -p 80 -i 16600 -l 60000 -m -q -o $RESULTS_DIR/fg_bg.log &
PID_BG=$!
nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
nice -n 19 dd if=/dev/urandom of=/dev/null bs=1M &
wait $PID_BG
tail -1 $RESULTS_DIR/fg_bg.log | tee -a $RESULTS_DIR/summary.txt

echo "" | tee -a $RESULTS_DIR/summary.txt
echo "=== Results saved to $RESULTS_DIR ===" | tee -a $RESULTS_DIR/summary.txt
```

---

## 五、风险与局限

### 5.1 slice 值需要调优

当前的 slice 映射(1ms/1.5ms/2ms/4ms/10ms/14ms)是经验值。实际效果取决于:
- 系统负载(任务数量、CPU 核心数)
- 任务行为(CPU-bound vs IO-bound)
- 工作负载类型(交互式 vs 批处理)

可能需要通过 cyclictest 测量后反复调整。

### 5.2 对 CPU-bound 任务提升有限

EEVDF+QoS 的优势主要体现在:
- 延迟敏感型任务(交互、渲染、音频)
- 前后台并发场景

对纯计算型任务(编译、推理、编码)的提升不明显,因为这些任务不关心调度延迟,只关心吞吐量。

### 5.3 与 RTG 的协同

RTG(Resource Thread Group)已有 VIP/TOP_TASK_KEY 的 RT 提升机制:
- VIP: SCHED_FIFO 优先级 97
- TOP_TASK_KEY: SCHED_FIFO 优先级 98

EEVDF+QoS 是在 CFS 层面的补充,两者可以叠加:
- RTG 负责帧级别的 RT 提升(硬件加速路径)
- EEVDF+QoS 负责 CFS 层面的优先级区分(软件通用路径)

### 5.4 QEMU 验证的局限

QEMU 验证只能证明:
- 内核不崩溃
- 基本功能正常(服务启动)

QEMU 无法证明:
- 调度延迟改善(需要真实硬件)
- 帧率稳定性(需要 GPU/显示硬件)
- 功耗影响(需要真实电源管理)

所有性能验证必须在 rk3568 实机上进行。
