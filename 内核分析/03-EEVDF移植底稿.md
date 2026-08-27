# EEVDF 移植底稿(6.6 → 5.10)

> 目标:把 OpenHarmony 6.6 内核(linux-6.6)的 EEVDF 调度模型移植到 5.10 内核(linux-5.10),并衔接 OHOS QoS 层(qos_manager)验证差异
> 源码: `~/OHOS501/kernel/linux/linux-6.6`(移植源)、`~/OHOS501/kernel/linux/linux-5.10`(移植目标)
> 依据: 6.6 fair.c 的 EEVDF 实现(源码行号已核实)

## 一、EEVDF 核心组件清单(6.6 fair.c,按移植顺序)

| 组件 | 6.6 位置 | 功能 | 移植类型 |
|---|---|---|---|
| `avg_vruntime_add/sub/update` | fair.c L674-708 | 维护 cfs_rq 加权平均虚拟时间 | 新增 |
| `avg_vruntime()` | fair.c ~L713 | 计算当前公平基线 V(带左偏取整) | 新增 |
| `update_entity_lag()` | fair.c L745 | 计算/钳制任务 lag: `lag = V - vruntime`,限制 ±max(2×slice, TICK) | 新增 |
| `entity_eligible()` | fair.c L773 | 判定任务"应得服务"(lag≥0),用整数不等式避免除法精度 | 新增 |
| `__update_min_vruntime/update_min_vruntime` | fair.c ~L790/L807 | min_vruntime 单调推进,同时 `avg_vruntime_update` 联动 | **改写**(5.10 版本无 avg 联动) |
| `min_deadline` 增强红黑树 | fair.c L830-880 | `RB_DECLARE_CALLBACKS` 维护子树最小 deadline,实现 O(log n) EDF 搜索 | 新增(依赖 rbtree 基础设施) |
| `__enqueue_entity/__dequeue_entity` | fair.c ~L868/878 | 入队时 `avg_vruntime_add` + 增强树插入 | **改写** |
| `__pick_eevdf/pick_eevdf` | fair.c L910/1004 | 两阶段堆搜索:eligible 树中找最早 deadline;失败回退最左 | 新增 |
| `update_deadline()` | fair.c L1056 | `deadline = vruntime + calc_delta_fair(slice, se)`,超限则 resched | 新增 |
| `update_curr()` | fair.c L1187 | 更新 vruntime 后 `update_deadline` 联动 | **改写** |
| `place_entity()` | fair.c L5100 | vlag 保真放置(PLACE_LAG)+ 初始半片(PLACE_DEADLINE_INITIAL) | **改写**(5.10 版是固定 sleep 补偿) |
| `set_next_entity()` | fair.c L5346 | HACK:pick 时把 deadline 存入 vlag | **改写** |
| `enqueue_entity()` | fair.c L5206 | 入队前 `update_entity_lag` | **改写** |
| `pick_next_entity()` | fair.c ~L5350 | `NEXT_BUDDY` 检查 + `pick_eevdf` | **改写**(5.10 版取最左+buddy) |
| `check_preempt_wakeup()` | fair.c L8312 | 唤醒抢占判定 | **改写**(可先保留 5.10 逻辑) |

## 二、数据结构改动

### sched_entity(include/linux/sched.h)
| 字段 | 6.6 | 5.10 | 说明 |
|---|---|---|---|
| `deadline` | ✓(L551) | ✗ | 虚拟截止时间 |
| `min_deadline` | ✓(L552) | ✗ | 子树最小 deadline(增强树缓存) |
| `vlag` | ✓(L560) | ✗ | 虚拟滞后(s64) |
| `slice` | ✓(L561) | ✗ | 请求时长片 |
| `vruntime/load/run_node` | ✓ | ✓ | 两版本共有,不动 |

### cfs_rq(kernel/sched/sched.h)
| 字段 | 6.6 | 5.10 |
|---|---|---|
| `avg_vruntime`(s64) | ✓(L585) | ✗ |
| `avg_load`(u64) | ✓(L586) | ✗ |

## 三、基础设施依赖(移植的隐藏成本)

1. **增强红黑树**: 6.6 的 `rb_add_augmented_cached` + `RB_DECLARE_CALLBACKS`(include/linux/rbtree_augmented.h)是 6.6 新增,5.10 没有。**需从 6.6 拷贝该头文件接口**(约 60 行,纯头文件,无其他依赖)
2. **u64_u32_store**: 6.6 为 32 位平台分字存取 min_vruntime。RK3568 是 arm64(CONFIG_64BIT),**可简化为 5.10 已有的 WRITE_ONCE,不必移植**
3. `scale_load_down`/`calc_delta_fair`/`entity_before`: 5.10 已有,直接可用
4. **sysctl 参数**: 6.6 用 `sysctl_sched_base_slice`(750000ns)替代了 5.10 的 `sched_min_granularity`;`sched_latency`/`sched_wakeup_granularity` 两版都有。移植时新增 base_slice,**保留 min_granularity 导出以兼容上层**(5.10 的 runtime 环境可能读它)
5. **features.h 新开关**: `PLACE_LAG`、`PLACE_DEADLINE_INITIAL`、`RUN_TO_PARITY`(6.6 默认均开启)——从 6.6 拷贝这几个特性定义

## 四、RTG 耦合分析(5.10 RK 特有,移植风险点)

5.10 中 RTG 模块与 fair.c 的交互点(已核实):
- `fair.c:25` `#include "rtg/rtg.h"`
- `fair.c:4291` `find_rtg_target(p)`(select_task_rq_fair 内,选择目标 CPU)
- `fair.c:7139` `find_rtg_cpu(p)`(task fork/迁移路径)
- `fair.c:11440` `find_rtg_target(p)`(负载均衡路径)
- `rtg/frame_rtg.c:903` 仅用 `trace_rtg_frame_sched`,不动 fair.c 内部

**结论**: RTG 的 hook 全部在"选 CPU"路径(select_task_rq_fair / 负载均衡),与 EEVDF 的"选任务/vruntime 机制"**正交**,移植不冲突。但注意:
- rtg.c 内部有自己的负载统计,`update_curr` 语义变化不影响它
- QoS 的 RTG ioctl 通道(concurrent_task_controller 的 RTG_SCHED_IPC)继续可用
- 唯一需检查:rtg 对 `place_entity` 睡眠补偿行为的隐式依赖(rtg.c 未引用 place_entity,已确认无)

## 五、工作量与风险

### 改动量估算(核心)
| 项 | 行数 |
|---|---|
| sched_entity 4 字段 + cfs_rq 2 字段 | ~30 |
| rbtree_augmented.h 接口 | ~60(拷贝) |
| fair.c 新函数(avg/lag/eligible/eevdf pick) | ~280 |
| fair.c 改写点(enqueue/dequeue/set_next/update_curr/place/pick) | ~200 |
| sysctl + features.h | ~30 |
| **合计** | **~600 行** |

### 风险清单
1. **中**: `update_curr` 的 min_vruntime 推进逻辑变了(avg 联动),5.10 其他调用点(约 10 处)不感知,但需逐处确认无依赖旧语义
2. **中**: CONFIG_FAIR_GROUP_SCHED 路径(组实体也有 vruntime/lag),6.6 是同一套代码天然支持;移植时组调度的 enqueue/place 调用链必须走新版本
3. **低**: cfs_bandwidth(throttle)路径调 enqueue_entity,参数与 6.6 一致即可
4. **低**: RTG(上文已分析,正交)
5. **验证要求高**: EEVDF 的数学错误不会崩机,只表现为"调度异常"——必须用 cyclictest/perf sched 对比 5.10 原版基线

## 六、移植后 QoS 联动验证方案(下一阶段)

1. **权重映射验证**: qos_policy.cpp 的权重表(1024/250/200/150)在 EEVDF 下直接作用于 vslice → 用 SetThreadQos 设置前后对比 `/proc/sched_debug` 的 deadline 分布
2. **抢占差异**: QOS_USER_INTERACTIVE 任务唤醒 vs 后台任务 → cyclictest 延迟分布对比(CFS vs EEVDF)
3. **DEADLINE_REQUEST 映射**(加分项): OHOS 的 QOS_DEADLINE_REQUEST 等级可直连 EEVDF 的 deadline 语义(6.6 上游论文思路),需要小改 qos_manager 或内核接口
4. 观测指标: PSI cpu、`/proc/schedstat` 切换次数、cyclictest p50/p95/p99、perf sched latency

## 七、执行顺序

1. [ ] 拷贝 rbtree_augmented.h 增强接口(6.6 → 5.10)
2. [ ] sched.h / include/linux/sched.h 加字段
3. [ ] fair.c: 新增 avg/lag/eligible/deadline/pick_eevdf 函数族
4. [ ] fair.c: 改写 update_curr → update_min_vruntime → enqueue/dequeue → place_entity → set_next_entity → pick_next_entity
5. [ ] sysctl(base_slice)+ features.h 开关
6. [ ] 编译 `./build.sh --product-name rk3568`(或 qemu 快速验证)
7. [ ] 基线对比(5.10 原版): cyclictest + perf sched + PSI
8. [ ] QoS 联动验证(第六节)