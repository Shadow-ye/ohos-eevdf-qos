# EEVDF 移植计划:Linux 6.6 → 5.10

> 目标:把 OpenHarmony 6.6 内核(linux-6.6)的 EEVDF 调度模型移植到 5.10 内核(linux-5.10)
> 目的:为 OHOS QoS 层(qos_manager)提供更精确的调度模型,验证 EEVDF + QoS 联动的差异
> 前提:本机已验证 5.10 与 6.6 两版源码,关键函数行号已核实

---

## 一、可行性分析

### 1.1 有利条件(移植可行的核心依据)

| 条件 | 证据 |
|---|---|
| 两版源码同源 | 本机 `~/OHOS501/kernel/linux/` 下 linux-5.10 与 linux-6.6 均来自 OpenHarmony 官方仓库 |
| 5.10 已有增强红黑树基础设施 | 5.10 `rbtree_augmented.h` 已有 `RB_DECLARE_CALLBACKS`、`rb_erase_augmented_cached`、`rb_insert_augmented_cached`,只缺 `rb_add_augmented_cached` |
| 5.10 已有 BASE_SLICE 特性 | 5.10 features.h 已有 `SCHED_FEAT(BASE_SLICE, true)`,用 `sysctl_sched_min_granularity` 替代 6.6 的 `sysctl_sched_base_slice`——只是命名差异 |
| 结构体改动最小化 | `sched_entity` 只加 4 个字段(dealine/min_deadline/vlag/slice),不改已有字段 |
| RTG 正交 | 5.10 RTG 的 hook(`find_rtg_target`/`find_rtg_cpu`)全在选 CPU 路径,与 vruntime 机制无耦合 |
| cfs_bandwidth 兼容 | `unthrottle_cfs_rq` 调 `enqueue_entity(ENQUEUE_WAKEUP)`,新 enqueue 逻辑兼容此 flag |

### 1.2 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| `update_curr` 语义变化(avg 联动 min_vruntime) | 中 | 逐处确认 5.10 的 10 个调用点无旧语义依赖 |
| 组调度(CONFIG_FAIR_GROUP_SCHED)路径 | 中 | 6.6 的组实体与 5.10 共享同一套代码路径,移植时同步改 group entity 的 enqueue/place |
| `entity_is_long_sleeper` 移除 | 低 | 6.6 用 vlag 替代,5.10 的 `entity_is_long_sleeper` 逻辑不再需要,直接删除 |
| 编译兼容性 | 低 | 6.6 新增依赖头文件少,逐个补齐即可 |
| 调度行为回归 | 中 | 先用 cyclictest 对比基线,再进 QoS 场景 |

### 1.3 可行性结论

**可行,且工作量比预期小。** 关键发现:5.10 已有 `BASE_SLICE` 特性和 `ALT_PERIOD`,rbtree 基础设施也基本完备,只缺 `rb_add_augmented_cached` 一个函数。核心改动集中在 `fair.c` 的 14 个函数,总改动量约 550 行。

---

## 二、移植文件清单(按执行顺序)

### 阶段 1:数据结构改动(~9 行,零风险)

| 文件 | 改动 |
|---|---|
| `include/linux/sched.h` | `sched_entity` 加 4 字段:`deadline`/`min_deadline`/`vlag`/`slice` |
| `kernel/sched/sched.h` | `cfs_rq` 加 2 字段:`avg_vruntime`(s64)/`avg_load`(u64) |
| `kernel/sched/features.h` | 加 3 个特性开关:`PLACE_LAG`/`PLACE_DEADLINE_INITIAL`/`RUN_TO_PARITY` |

### 阶段 2:基础设施补丁(~35 行,低风险)

| 文件 | 改动 |
|---|---|
| `include/linux/rbtree_augmented.h` | 从 6.6 拷贝 `rb_add_augmented_cached()` 函数 |

### 阶段 3:EEVDF 核心新函数(~245 行,零风险)

| 函数 | 行数 | 功能 |
|---|---|---|
| `avg_vruntime_add/sub/update` | ~30 | 维护 cfs_rq 加权平均虚拟时间 |
| `avg_vruntime()` | ~20 | 计算当前公平基线 V |
| `update_entity_lag()` | ~15 | 计算/钳制 lag |
| `entity_eligible()` | ~20 | 判定任务是否"应得服务" |
| `__pick_eevdf()/pick_eevdf()` | ~105 | 堆搜索:eligible 树中找最早 deadline |
| `update_deadline()` | ~20 | deadline = vruntime + calc_delta_fair(slice, se) |
| `__entity_less()` | ~5 | 红黑树比较回调 |
| `min_deadline_update` + `RB_DECLARE_CALLBACKS(min_deadline_cb)` | ~30 | 增强红黑树 min_deadline 维护 |

### 阶段 4:改写已有函数(~260 行,核心改动)

| 函数 | 6.6 行号 | 改动内容 |
|---|---|---|
| `update_curr()` | L1187 | 更新 vruntime 后加 `update_deadline(cfs_rq, se)` |
| `update_min_vruntime()` | ~L807 | 加 `avg_vruntime_update(cfs_rq, delta)` 联动 |
| `__enqueue_entity()` | L868 | 改用 `avg_vruntime_add` + `rb_add_augmented_cached` |
| `__dequeue_entity()` | L878 | 改用 `rb_erase_augmented_cached` + `avg_vruntime_sub` |
| `enqueue_entity()` | L5206 | 入队前调 `update_entity_lag` |
| `set_next_entity()` | L5346 | pick 时把 deadline 存入 vlag |
| `pick_next_entity()` | ~L5350 | 改用 `pick_eevdf` |
| `place_entity()` | L5100 | 全面改写为 vlag 保真放置(替换旧版 sleep 补偿) |
| `check_preempt_wakeup()` | L8312 | 加 `entity_eligible` 判定,可先保留旧逻辑 |
| `dequeue_entity()` | L5293 | 确保 vlag 保留(不再清零) |
| `update_curr_fair()` | L1228 | 检查是否需联动(小改) |

### 阶段 5:sysctl 参数与清理(~20 行)

| 改动 | 说明 |
|---|---|
| `sched_slice()` | 5.10 用 `sysctl_sched_min_granularity` 作为 base_slice(已是 BASE_SLICE 语义),无需改 |
| `entity_is_long_sleeper()` | 删除(6.6 不再使用,vlag 替代) |
| `GENTLE_FAIR_SLEEPERS` | 保留(6.6 也保留,但 EEVDF 下影响减弱) |
| `START_DEBIT` | 保留(6.6 也保留) |

---

## 三、文件汇总

| 文件 | 改动量 | 风险 |
|---|---|---|
| `include/linux/sched.h` | +4 字段 | 零 |
| `kernel/sched/sched.h` | +2 字段 | 零 |
| `kernel/sched/features.h` | +3 开关 | 零 |
| `include/linux/rbtree_augmented.h` | +35 行(新函数) | 低 |
| `kernel/sched/fair.c` | ~505 行(新函数+改写) | 中 |
| `kernel/sched/features.h`(已含) | - | - |
| **合计** | **~550 行** | **中(可控)** |

---

## 四、QoS 联动验证方案

### 4.1 权重映射验证

OHOS QoS 的 `qos_policy.cpp` 权重表:
```
QOS_USER_INTERACTIVE → weight=200(nice=-10)
QOS_USER_INITIATED   → weight=250(nice=-5)
QOS_DEFAULT          → weight=1024(nice=0)
QOS_BACKGROUND       → weight=150(nice=+15)
```

EEVDF 下:权重 → vslice,直接影响 deadline 计算。验证方法:
1. `SetThreadQos(QOS_USER_INTERACTIVE)` + `SetThreadQos(QOS_BACKGROUND)`
2. 对比 `/proc/sched_debug` 的 deadline 分布
3. cyclictest 延迟分布(CFS vs EEVDF)

### 4.2 唤醒抢占差异

| 维度 | CFS + QoS | EEVDF + QoS |
|---|---|---|
| 高 QoS 任务唤醒 | wakeup_gran 1ms 阈值 | entity_eligible + deadline 判定,抢占更果断 |
| 低 QoS 任务睡眠后 | sleep 补偿可能插队 | vlag 校准,lag<0 严格排队 |
| DEADLINE_REQUEST 等级 | 只能靠权重近似 | 可映射 EEVDF 的 deadline 语义 |

### 4.3 验证指标

- cyclictest p50/p95/p99 延迟
- perf sched latency
- `/proc/pressure/cpu`(PSI)
- `/proc/schedstat` 切换次数
- `vmstat` 上下文切换计数

---

## 五、执行时间表

| 阶段 | 内容 | 预计耗时 | 前置条件 |
|---|---|---|---|
| 1 | 数据结构改动(sched.h/features.h) | 10 分钟 | 无 |
| 2 | rbtree_augmented.h 补丁 | 10 分钟 | 无 |
| 3 | EEVDF 核心新函数 | 1-2 小时 | 阶段 1-2 |
| 4 | 改写已有函数 | 2-3 小时 | 阶段 3 |
| 5 | 编译验证+清理 | 1-2 小时 | 阶段 4 |
| 6 | cyclictest 基线对比 | 1 小时 | 阶段 5 |
| 7 | QoS 联动验证 | 2-3 小时 | 阶段 6 |
| **合计** | | **8-12 小时** | |

---

## 六、退出策略

如果 EEVDF 移植遇到不可克服的编译/运行问题,可退守:
1. **方案 B:部分借鉴**——只移植 `place_entity` 的 vlag 校准逻辑(替换旧版 sleep 补偿),保留 CFS 的 vruntime 红黑树。改动 ~100 行,风险极低,仍有"改进调度放置策略"的答辩点
2. **方案 C:纯参数优化**——利用 5.10 已有的 `features.h` 开关(GENTLE_FAIR_SLEEPERS/WAKEUP_PREEMPTION 等)做实验性调整,配 QoS 联动验证

---

## 七、决策依据(为什么选 EEVDF 而非纯调参)

| 维度 | 纯调参 | EEVDF 移植 |
|---|---|---|
| 深度 | 改一行开关 | 引入下一代调度模型 |
| 答辩 | "我调了参数" | "我把 6.6 的 EEVDF 移植到 5.10,验证了 QoS 联动" |
| 创新性 | 竞品都在做 | 罕见(5.10 + EEVDF 的组合几乎无人做过) |
| 可量化 | PSI/延迟微小变化 | 公平性/延迟边界数学保证 |
| 风险 | 低 | 中(但有退出策略) |