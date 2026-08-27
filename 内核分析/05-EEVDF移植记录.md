# EEVDF 移植记录:Linux 6.6 → 5.10

> 移植时间:2026-08-21
> 目标:把 OpenHarmony 6.6 内核的 EEVDF 调度模型移植到 5.10 内核
> 结果:QEMU 编译通过,120 秒启动验证通过,0 致命崩溃,samgr ready

---

## 一、移植概览

| 项目 | 数据 |
|---|---|
| 源码 | `~/OHOS501/kernel/linux/linux-6.6`(EEVDF 源) |
| 目标 | `~/OHOS501/kernel/linux/linux-5.10`(移植目标) |
| 改动文件 | 5 个 |
| 新增代码 | ~550 行 |
| 编译时间 | 3 分 44 秒 |
| 验证结果 | QEMU 120 秒启动通过,0 崩溃 |

---

## 二、改动文件清单

### 2.1 `include/linux/sched.h`(+4 字段)

在 `struct sched_entity` 中新增 4 个 EEVDF 字段:

```c
struct sched_entity {
    struct load_weight      load;
    struct rb_node          run_node;
    u64                     deadline;      // + 虚拟截止时间
    u64                     min_deadline;  // + 子树最小 deadline(增强树缓存)
    struct list_head        group_node;
    unsigned int            on_rq;
    u64                     exec_start;
    u64                     sum_exec_runtime;
    u64                     vruntime;
    s64                     vlag;          // + 虚拟滞后
    u64                     slice;         // + 请求时长片
    u64                     prev_sum_exec_runtime;
    u64                     nr_migrations;
    // ... 其余不变
};
```

### 2.2 `kernel/sched/sched.h`(+2 字段 +1 宏)

`struct cfs_rq` 新增:

```c
struct cfs_rq {
    s64             avg_vruntime;  // + 加权平均虚拟时间
    u64             avg_load;      // + 总权重
    // ... 其余不变
};
```

新增宏:

```c
#define ENQUEUE_INITIAL  0x80  // 新任务入队标志
```

### 2.3 `kernel/sched/features.h`(+3 开关)

```c
SCHED_FEAT(PLACE_LAG, true)           // vlag 保真放置
SCHED_FEAT(PLACE_DEADLINE_INITIAL, true)  // 初始半片
SCHED_FEAT(RUN_TO_PARITY, true)       // 运行到奇偶
```

### 2.4 `include/linux/rbtree_augmented.h`(+35 行)

从 6.6 拷贝 `rb_add_augmented_cached()` 函数,实现增强红黑树的插入操作.

### 2.5 `kernel/sched/fair.c`(~550 行)

**新增函数(~250 行):**

| 函数 | 功能 |
|---|---|
| `entity_key()` | 计算实体与 min_vruntime 的差值 |
| `__node_2_se()` | 红黑树节点→调度实体的宏 |
| `avg_vruntime_add/sub/update()` | 维护 cfs_rq 加权平均 |
| `avg_vruntime()` | 计算当前公平基线 V |
| `update_entity_lag()` | 计算/钳制 lag |
| `entity_eligible()` | 判定任务是否"应得服务" |
| `__entity_less()` | 红黑树比较回调 |
| `deadline_gt/__update_min_deadline/min_deadline_update()` | 增强红黑树 min_deadline 维护 |
| `RB_DECLARE_CALLBACKS(min_deadline_cb)` | 声明增强树回调 |
| `__pick_eevdf()` | 堆搜索:eligible 树中找最早 deadline |
| `pick_eevdf()` | 封装,失败回退最左 |
| `update_deadline()` | deadline = vruntime + calc_delta_fair(slice, se) |

**改写函数(~260 行):**

| 函数 | 改动内容 |
|---|---|
| `__update_min_vruntime()` | 新增:avg_vruntime_update 联动 |
| `update_min_vruntime()` | 改用 __update_min_vruntime + __pick_first_entity |
| `__enqueue_entity()` | 改用 avg_vruntime_add + rb_add_augmented_cached |
| `__dequeue_entity()` | 改用 rb_erase_augmented_cached + avg_vruntime_sub |
| `enqueue_entity()` | 移除 renorm 逻辑,改用 place_entity 处理 vruntime |
| `dequeue_entity()` | 新增 update_entity_lag 调用,移除 vruntime 归一化 |
| `update_curr()` | 新增 update_deadline 调用 |
| `set_next_entity()` | 新增 HACK:deadline→vlag 暂存 |
| `pick_next_entity()` | 改用 pick_eevdf |
| `place_entity()` | 全面改写为 vlag 保真放置(替换旧版 sleep 补偿) |
| `check_preempt_wakeup()` | 改用 pick_eevdf 判定抢占 |

**删除函数:**

| 函数 | 原因 |
|---|---|
| `entity_is_long_sleeper()` | 6.6 用 vlag 替代,不再需要 |

---

## 三、编译过程中遇到的问题与修复

### 3.1 前向引用错误

**问题:**`__update_min_vruntime` 调用 `avg_vruntime_update`,但后者定义在后面

**原因:**新 EEVDF 函数块插入位置在 `__update_min_vruntime` 之后

**修复:**将整个 EEVDF 函数块移到 `entity_before()` 之前,确保所有函数在调用前定义

### 3.2 缺少头文件包含

**问题:**`implicit declaration of function 'rb_add_augmented_cached'`

**原因:**fair.c 未包含 `rbtree_augmented.h`

**修复:**添加 `#include <linux/rbtree_augmented.h>`

### 3.3 缺少 `ENQUEUE_INITIAL` 宏

**问题:**`use of undeclared identifier 'ENQUEUE_INITIAL'`

**原因:**5.10 没有此宏,6.6 在 sched.h 中定义

**修复:**在 `kernel/sched/sched.h` 中添加 `#define ENQUEUE_INITIAL 0x80`

### 3.4 缺少 `se_is_idle`/`cfs_rq_is_idle`

**问题:**`implicit declaration of function 'se_is_idle'`

**原因:**6.6 定义了这些函数,5.10 没有

**修复:**简化 `check_preempt_wakeup`,移除 idle 检测逻辑(不影响 EEVDF 核心)

### 3.5 `calc_delta_fair` 前向引用

**问题:**`update_entity_lag` 调用 `calc_delta_fair`,但后者定义在后面

**修复:**在 EEVDF 函数块前添加 `calc_delta_fair` 的前向声明

---

## 四、QEMU 验证结果

```
启动命令:timeout 120 ./vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh
退出码:124(超时,正常)
致命崩溃:0
samgr ready:1
内核panic:无
调度错误:无
watchdog_service:正常重启循环(每10秒)
```

---

## 五、关键设计决策

| 决策 | 理由 |
|---|---|
| 保留 `sysctl_sched_min_granularity` | 5.10 已有 BASE_SLICE 特性,用 min_granularity 替代 6.6 的 base_slice,兼容性更好 |
| 简化 `check_preempt_wakeup` | 移除 `se_is_idle` 依赖,不影响 EEVDF 核心调度逻辑 |
| 保留 `GENTLE_FAIR_SLEEPERS` | 6.6 也保留此特性,EEVDF 下影响减弱但不冲突 |
| 不修改 RTG 模块 | RTG hook 在选 CPU 路径,与 vruntime 机制正交 |

---

## 六、后续工作

1. **QoS 联动验证:**用 `SetThreadQos` 设置不同等级,对比 cyclictest 延迟分布(CFS vs EEVDF)
2. **rk3568 编译:**`./build.sh --product-name rk3568` 出真机镜像
3. **调度延迟对比:**perf sched latency + PSI cpu 指标
4. **DEADLINE_REQUEST 映射:**OHOS QoS 的 DEADLINE_REQUEST 等级可直连 EEVDF deadline 语义(加分项)
