# EEVDF 与 OHOS QoS 适配:从原理到实现

> 日期:2026-08-21
> 内核:linux-5.10 (OHOS 5.0.1)
> 结果:QEMU 编译通过,120 秒启动验证通过,0 崩溃

---

## 目录

1. [背景:什么是 EEVDF 和 QoS](#一背景)
2. [问题:为什么需要适配](#二问题)
3. [架构:数据如何流动](#三架构)
4. [实现:代码逐行解析](#四实现)
5. [配置:如何启用](#五配置)
6. [验证:如何测试](#六验证)
7. [扩展:后续可以做什么](#七扩展)

---

## 一、背景

### 1.1 什么是 EEVDF

EEVDF(Earliest Eligible Virtual Deadline First)是 Linux 6.6 引入的公平调度器,替代 CFS(Completely Fair Scheduler)。

**CFS 的工作方式:**

```
每个任务有一个 vruntime(虚拟运行时间)
vruntime 增速 = 实际运行时间 × (nice_0_weight / 当前任务权重)
调度器总是选 vruntime 最小的任务运行
```

CFS 只有一个维度:**谁运行得少,谁先运行**。

**EEVDF 的工作方式:**

```
EEVDF 在 CFS 基础上增加了 virtual deadline(虚拟截止时间)

每个任务有两个属性:
  1. vruntime: 谁运行得少(公平性)
  2. deadline: 谁更紧急(时效性)

调度规则:在所有 eligible(已过期)任务中,选 deadline 最早的
```

EEVDF 有两个维度:**谁运行得少** + **谁更紧急**。

**关键公式:**

```
deadline = vruntime + calc_delta_fair(slice, weight)

其中:
  slice  = 任务请求的运行时长(时间片)
  weight = 任务的权重(由 nice 值决定)
  calc_delta_fair = 将真实时间转换为虚拟时间的函数
```

**直觉理解:**
- slice 短 → deadline 早 → 在 EEVDF 红黑树中优先级高
- slice 长 → deadline 晚 → 优先级低
- weight 高 → vruntime 增速慢 → 更容易 eligible

### 1.2 什么是 OHOS QoS

OHOS(OpenHarmony)有一个 QoS(Quality of Service)系统,允许应用声明线程的优先级:

```cpp
// 用户空间 API
enum class QosLevel {
    QOS_BACKGROUND,        // 0 - 后台任务
    QOS_UTILITY,           // 1 - 工具型任务
    QOS_DEFAULT,           // 2 - 默认
    QOS_USER_INITIATED,    // 3 - 用户主动触发
    QOS_DEADLINE_REQUEST,  // 4 - 截止时间请求
    QOS_USER_INTERACTIVE,  // 5 - 用户交互(最高)
    QOS_KEY_BACKGROUND,    // 6 - 关键后台
};

// 使用方式
SetThreadQos(QOS_USER_INTERACTIVE);  // 设置当前线程为最高优先级
```

**QoS 如何映射到内核参数:**

```
用户空间                    内核空间
─────────                  ─────────
QOS_USER_INTERACTIVE  →  nice = -10,  latency_nice = -10
QOS_DEFAULT           →  nice = 0,    latency_nice = 0
QOS_BACKGROUND        →  nice = 10,   latency_nice = 10
```

### 1.3 两者如何协作

在 CFS 下:
```
QoS → nice → weight → vruntime 增速 → 调度优先级(唯一维度)
```

在 EEVDF 下(适配前):
```
QoS → nice → weight → vruntime 增速 → 调度优先级(之一)
                  ↑
                  slice 被忽略(硬编码为相同值)
```

在 EEVDF 下(适配后):
```
QoS → nice → weight → vruntime 增速 → 调度优先级(之一)
QoS → in_qos → slice → deadline → 调度优先级(之二)
```

---

## 二、问题

### 2.1 问题的本质

在我们移植的 EEVDF 中,slice 在两个地方被硬编码:

```c
// 位置1: fair.c:4582 - place_entity() - 任务入队时
se->slice = sysctl_sched_min_granularity;  // 所有任务相同

// 位置2: fair.c:857 - update_deadline() - deadline 过期时
se->slice = sysctl_sched_min_granularity;  // 所有任务相同
```

`sysctl_sched_min_granularity` 默认值是 750000 纳秒(0.75 毫秒),所有任务都用这个值。

**结果:**所有任务的 slice 相同 → deadline 仅由 weight 决定 → QoS 等级只能通过 nice(权重)间接影响 EEVDF。

### 2.2 为什么 latency_nice 不够

OHOS QoS 已经设置了 `latency_nice`(通过 `SCHED_FLAG_LATENCY_NICE`)。但 latency_nice 在5.10 内核中的作用有限:

```c
// fair.c:7468 - 只影响唤醒时的 preemption 判定
static long wakeup_latency_gran(struct sched_entity *curr,
                                 struct sched_entity *se)
{
    int latency_weight = se->latency_weight;
    // ... 只在唤醒瞬间起作用
}
```

**latency_nice 只在唤醒瞬间起作用**,一旦任务进入运行队列,后续的 EEVDF 调度完全由 slice/deadline 决定。

### 2.3 问题的具体表现

假设前台渲染线程和后台日志线程同时竞争 CPU:

| 场景 | CFS | EEVDF(未适配) | EEVDF(适配后) |
|---|---|---|---|
| 渲染线程 nice=-10 | 权重高,优先 | 权重高,但 deadline 相同 | 权重高 + **deadline 早** |
| 日志线程 nice=10 | 权重低,延后 | 权重低,但 deadline 相同 | 权重低 + **deadline 晚** |
| 延迟差异 | 明显 | **不明显** | **非常明显** |

---

## 三、架构

### 3.1 数据通路全景

```
┌─────────────────────────────────────────────────────────────┐
│                      用户空间                               │
│                                                             │
│  SetThreadQos(QOS_USER_INTERACTIVE)                        │
│       │                                                     │
│       ▼                                                     │
│  ioctl(/proc/thread-self/sched_qos_ctrl)                   │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                    内核 QoS 控制层                           │
│  kernel/linux/common_modules/qos_auth/auth_ctl/qos_ctrl.c  │
│                                                             │
│  ctrl_qos_operation()                                       │
│    → qos_apply()                                            │
│      → sched_set_task_qos_attr(p, level, status)           │
│        │                                                    │
│        ├─ 读取 policy_map->levels[level]                   │
│        │   设置 nice, latency_nice, uclamp                 │
│        │                                                    │
│        ├─ sched_setattr_nocheck(p, &attr)                  │
│        │   → p->se.load.weight = weight_of_nice(nice)     │
│        │   → p->latency_prio = NICE_TO_LATENCY(...)       │
│        │                                                    │
│        └─ qts->in_qos = level                              │
│           (QoS 等级存储在 task_struct 中)                   │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│                  内核 EEVDF 调度器                           │
│  kernel/linux/linux-5.10/kernel/sched/fair.c               │
│                                                             │
│  place_entity() / update_deadline()                         │
│    │                                                        │
│    ├─ 读取 task->qts.in_qos                                │
│    │                                                        │
│    ├─ qos_to_slice(in_qos) → slice                         │
│    │                                                        │
│    └─ deadline = vruntime + calc_delta_fair(slice, se)     │
│                                                             │
│  pick_eevdf()                                               │
│    → 选 deadline 最早的 eligible 任务                       │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 关键数据结构

**task_struct 中的 QoS 信息:**

```c
// include/linux/sched.h:755
struct qos_task_struct {
    int                 in_qos;     // QoS 等级 [0, NR_QOS)
    struct list_head    qos_list;   // 链表节点
};

// include/linux/sched.h:1516
struct task_struct {
    // ...
#ifdef CONFIG_QOS_CTRL
    struct qos_task_struct qts;     // ← QoS 信息在这里
#endif
    // ...
};
```

**sched_entity 中的 EEVDF 信息:**

```c
// include/linux/sched.h:518
struct sched_entity {
    struct load_weight  load;           // 权重(由 nice 决定)
    struct rb_node      run_node;       // 红黑树节点
    u64                 deadline;       // 虚拟截止时间
    u64                 vruntime;       // 虚拟运行时间
    u64                 slice;          // ← 我们要改的地方
    s64                 vlag;           // 虚拟滞后
    // ...
};
```

### 3.3 QoS 等级到 slice 的映射

```
QoS 等级       nice    slice    deadline 效果
─────────────────────────────────────────────
KEY_BACKGROUND   0      4ms     同 DEFAULT
BACKGROUND      10     14ms     最晚
UTILITY          5     10ms     晚
DEFAULT          0      4ms     基线
USER_INITIATED -10      2ms     早
DEADLINE_REQ   -10    1.5ms     很早
USER_INTERACT  -10      1ms     最早,最高优先级
```

**为什么这样设计:**
- 高 QoS 等级(如 USER_INTERACTIVE)需要快速响应 → slice 短 → deadline 早
- 低 QoS 等级(如 BACKGROUND)允许更长延迟 → slice 长 → deadline 晚
- nice 值和 slice 叠加作用:高 QoS = 权重高 + deadline 早 = 双重优势
- KEY_BACKGROUND(6)在 `qos_manager` 的每套策略里都是 nice 0、不走 RT,和 DEFAULT 相同,所以 slice 也取 DEFAULT 的 4ms。最初的 14ms 和当天改过的 0.5ms 都已撤回,理由见 `09-EEVDF审查交接.md` 第 10 节

---

## 四、实现

### 4.1 改动1:添加 qos_to_slice() 映射函数

**文件:**`kernel/sched/fair.c`
**位置:**第 572-613 行(在 EEVDF helpers 之前)
**行数:**~42 行

```c
#ifdef CONFIG_QOS_CTRL
/*
 * OHOS QoS → EEVDF slice mapping
 *
 * In EEVDF, slice determines virtual deadline:
 *   deadline = vruntime + calc_delta_fair(slice, se)
 *
 * Shorter slice → earlier deadline → higher scheduling priority
 * Longer slice  → later deadline  → lower scheduling priority
 *
 * This allows QoS levels to express urgency through deadline placement,
 * complementing the weight-based priority from nice values.
 *
 * NR_QOS = 7 (from qos_ctrl.h)
 */
#define QOS_NR_LEVELS 7

static inline u64 qos_to_slice(int in_qos)
{
    /*
     * Slice values in nanoseconds. Levels 0-5 get shorter slices as the
     * level rises. Tasks without a QoS level keep
     * sysctl_sched_min_granularity.
     *
     * QOS_KEY_BACKGROUND is not above QOS_USER_INTERACTIVE: every
     * qos_manager policy gives it nice 0 and no RT, the same as
     * QOS_DEFAULT, so it shares QOS_DEFAULT's slice.
     */
    static const u64 qos_slice_map[QOS_NR_LEVELS] = {
        14000000ULL,   /* [0] QOS_BACKGROUND:       14ms */
        10000000ULL,   /* [1] QOS_UTILITY:          10ms */
         4000000ULL,   /* [2] QOS_DEFAULT:            4ms */
         2000000ULL,   /* [3] QOS_USER_INITIATED:     2ms */
         1500000ULL,   /* [4] QOS_DEADLINE_REQUEST: 1.5ms */
         1000000ULL,   /* [5] QOS_USER_INTERACTIVE:   1ms */
         4000000ULL,   /* [6] QOS_KEY_BACKGROUND:     4ms */
    };

    if (in_qos >= 0 && in_qos < QOS_NR_LEVELS)
        return qos_slice_map[in_qos];
    return sysctl_sched_min_granularity;
}
#endif /* CONFIG_QOS_CTRL */
```

**逐行解释:**

| 行 | 解释 |
|---|---|
| `#ifdef CONFIG_QOS_CTRL` | 只在 QoS 模块启用时编译,避免未启用时报错 |
| `#define QOS_NR_LEVELS 7` | QoS 等级数量,与 qos_ctrl.h 中的 NR_QOS 一致 |
| `static const u64 qos_slice_map[]` | 静态常量表,编译时确定,零运行时开销 |
| `14000000ULL` | 14 毫秒 = 14,000,000 纳秒(ULL = unsigned long long) |
| `in_qos >= 0 && in_qos < QOS_NR_LEVELS` | 边界检查,防止数组越界 |
| `sysctl_sched_min_granularity` | 未设置 QoS 时的默认值(0.75ms) |

### 4.2 改动2:修改 place_entity()

**文件:**`kernel/sched/fair.c`
**位置:**第 4626-4641 行
**改动:**将硬编码的 slice 赋值替换为 QoS 映射

```c
// 改动前:
se->slice = sysctl_sched_min_granularity;
vslice = calc_delta_fair(se->slice, se);

// 改动后:
if (entity_is_task(se)) {
#ifdef CONFIG_QOS_CTRL
    se->slice = qos_to_slice(task_of(se)->qts.in_qos);
#else
    se->slice = sysctl_sched_min_granularity;
#endif
} else {
    se->slice = sysctl_sched_min_granularity;
}
vslice = calc_delta_fair(se->slice, se);
```

**逐行解释:**

| 行 | 解释 |
|---|---|
| `entity_is_task(se)` | 判断是任务还是调度组(CFS group scheduling) |
| `task_of(se)` | 从调度实体获取 task_struct 指针 |
| `task_of(se)->qts.in_qos` | 读取任务的 QoS 等级(0-6) |
| `qos_to_slice(...)` | 将 QoS 等级转换为 slice 值 |
| `#else` 分支 | CONFIG_QOS_CTRL 未启用时,回退到默认值 |
| `else` 分支 | 调度组(非任务)使用默认值 |

**为什么需要 `entity_is_task()` 判断:**
- EEVDF 支持 CFS group scheduling(组调度)
- 组调度中,sched_entity 可能属于一个调度组,而不是一个任务
- 只有任务才有 `task_struct`,才有 `qts.in_qos`
- 调度组没有 QoS 信息,使用默认 slice

### 4.3 改动3:修改 update_deadline()

**文件:**`kernel/sched/fair.c`
**位置:**第 889-921 行
**改动:**与 place_entity() 相同的逻辑

```c
static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if ((s64)(se->vruntime - se->deadline) < 0)
        return;

    /*
     * For EEVDF the virtual time slope is determined by w_i (iow.
     * nice) while the request time r_i is determined by QoS level
     * via qos_to_slice(), falling back to sysctl_sched_min_granularity.
     */
    if (entity_is_task(se)) {
#ifdef CONFIG_QOS_CTRL
        se->slice = qos_to_slice(task_of(se)->qts.in_qos);
#else
        se->slice = sysctl_sched_min_granularity;
#endif
    } else {
        se->slice = sysctl_sched_min_granularity;
    }

    /*
     * EEVDF: vd_i = ve_i + r_i / w_i
     */
    se->deadline = se->vruntime + calc_delta_fair(se->slice, se);

    /*
     * The task has consumed its request, reschedule.
     */
    if (cfs_rq->nr_running > 1) {
        resched_curr(rq_of(cfs_rq));
        clear_buddies(cfs_rq, se);
    }
}
```

**为什么 update_deadline() 也需要改:**

`update_deadline()` 在任务的 deadline 过期时被调用。此时需要重新计算 deadline,而新的 deadline 仍然依赖 slice。如果这里不改,任务运行一段时间后,slice 会被重置为默认值,QoS 映射就失效了。

### 4.4 改动4:启用 CONFIG_QOS_CTRL

**文件:**`arch/arm64/configs/qemu-arm-linux_standard_defconfig`
**改动:**添加两行配置

```
CONFIG_AUTHORITY_CTRL=y
CONFIG_QOS_CTRL=y
```

**依赖关系:**

```
CONFIG_QOS_CTRL
  └─ depends on CONFIG_AUTHORITY_CTRL  (必须先启用)
       └─ tristate "Authority Control for RTG & QOS"
```

**为什么需要启用:**
- `CONFIG_QOS_CTRL` 控制 `struct qos_task_struct` 是否编译进内核
- 如果未启用,`task->qts` 不存在,编译会报错
- `CONFIG_AUTHORITY_CTRL` 是 `QOS_CTRL` 的依赖项

---

## 五、配置

### 5.1 defconfig 位置

```
kernel/linux/config/linux-5.10/arch/arm64/configs/qemu-arm-linux_standard_defconfig
```

### 5.2 需要添加的配置

在 defconfig 末尾添加:

```
CONFIG_AUTHORITY_CTRL=y
CONFIG_QOS_CTRL=y
```

### 5.3 验证配置是否生效

```bash
# 检查最终 .config
grep "QOS_CTRL\|AUTHORITY_CTRL" out/KERNEL_OBJ/kernel/OBJ/linux-5.10/.config

# 预期输出:
# CONFIG_AUTHORITY_CTRL=y
# CONFIG_QOS_CTRL=y
```

### 5.4 验证模块是否编译

```bash
# 检查 qos_ctrl.o 是否生成
find out -name "qos_ctrl.o"

# 预期输出:
# out/kernel/OBJ/linux-5.10/drivers/auth_ctl/qos_ctrl.o
```

---

## 六、验证

### 6.1 编译验证

```bash
cd ~/OHOS501
echo '3' | sudo -S ./build.sh --product-name qemu-arm64-linux-min --ccache
```

预期:编译成功,无 error

### 6.2 启动验证

```bash
echo '3' | sudo -S timeout 120 ./vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh
```

预期:
- exit code: 124 (timeout,正常)
- `critical service crashed`: 0
- `samgr ready ret`: 1
- 无内核 panic

### 6.3 功能验证(需要实机)

在实机上验证 QoS 等级是否影响调度延迟:

```c
// 测试程序
#include <sched.h>
#include <unistd.h>

int main() {
    // 设置最高 QoS
    SetThreadQos(QOS_USER_INTERACTIVE);

    // 运行 CPU 密集型任务
    while (1) {
        // ...
    }
}
```

对比不同 QoS 等级下的调度延迟分布。

### 6.4 性能对比

| 指标 | CFS(基线) | EEVDF(未适配) | EEVDF+QoS(适配后) |
|---|---|---|---|
| USER_INTERACTIVE 延迟 | 测量 | 测量 | 测量 |
| BACKGROUND 延迟 | 测量 | 测量 | 测量 |
| 延迟比值 | < 1 | ≈ 1 | << 1 |

---

## 七、扩展

### 7.1 sysctl 运行时调整

可以添加 sysctl 参数,允许运行时修改 slice 映射值:

```c
// kernel/sched/sched.h
extern unsigned int sysctl_sched_qos_slice[QOS_NR_LEVELS];
```

```bash
# 运行时调整
echo "1000000 2000000 4000000 2000000 1500000 1000000 1000000" \
    > /proc/sys/kernel/sched_qos_slice
```

### 7.2 用户自定义 slice

扩展 QoS API,允许用户指定自定义 slice:

```cpp
// 扩展 API
int SetThreadQosWithSlice(enum QosLevel level, uint64_t slice_ns);
```

### 7.3 与 SCHED_DEADLINE 集成

QOS_DEADLINE_REQUEST 可以直接映射到 SCHED_DEADLINE 调度策略,获得硬件级的实时保障。

### 7.4 统计与调试

添加 /proc 接口,暴露 QoS → slice 映射的运行时统计:

```bash
cat /proc/sched_qos_stats
# QOS_USER_INTERACTIVE: avg_slice=1ms, avg_deadline=2ms, nr_tasks=5
# QOS_BACKGROUND: avg_slice=14ms, avg_deadline=20ms, nr_tasks=12
```

---

## 附录:改动文件完整清单

| 文件 | 改动类型 | 行数 |
|---|---|---|
| `kernel/linux/linux-5.10/kernel/sched/fair.c` | 新增 qos_to_slice() | ~45 行 |
| `kernel/linux/linux-5.10/kernel/sched/fair.c` | 修改 place_entity() | ~10 行 |
| `kernel/linux/linux-5.10/kernel/sched/fair.c` | 修改 update_deadline() | ~10 行 |
| `kernel/linux/config/linux-5.10/arch/arm64/configs/qemu-arm-linux_standard_defconfig` | 启用 CONFIG_QOS_CTRL | 2 行 |

**总计:**~67 行改动
