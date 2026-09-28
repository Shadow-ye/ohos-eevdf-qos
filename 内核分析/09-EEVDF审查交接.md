# EEVDF 移植审查交接

> 审查日期：2026-09-28
> 修复日期：2026-09-28。第 3–8 节都已改完，改法和第二轮复查补的修复见第 10、11 节。代码在 `repo/kernel/linux/linux-5.10`，与本地 `OHOS501` 同名文件一致。`CONFIG_RTG_LATENCY_FUSION` 已从本地 `OHOS501` 的 `rtg.c` / `Kconfig` 删掉；远程仓库本来就没有这份 RTG 改动。
> 对照源：`OHOS501/kernel/linux/linux-6.6/kernel/sched/fair.c`（本树里的 6.6，不是更后面的主线修复）
> 审查对象：`OHOS501/kernel/linux/linux-5.10`
> 审查时的快照：`repo/kernel/linux/linux-5.10`，commit `d3263fb`（`feat: EEVDF scheduler migration`）
>
> 第 1–9 节是审查当时的描述，行号和“5.10 还是 CFS 写法”这类说法都指 `d3263fb`，不是现在的代码。

120 秒 QEMU 启动通过，不能说明下面的问题不存在。启动路径几乎不改权重、不调用 `sched_yield()`，也测不到 QoS 切片是否真的决定抢占点。

配置更正：审查时写的“QEMU defconfig 没有打开 `CONFIG_CGROUP_SCHED` / `CONFIG_SCHED_HRTICK`”不对。`qemu-arm-linux_standard_defconfig` 打开了 `CONFIG_SCHED_AUTOGROUP`，它选中 `CGROUP_SCHED` 和 `FAIR_GROUP_SCHED`；`SCHED_HRTICK` 在打开高精度定时器时默认是 `y`。实际编出的 `.config` 三项都是 `y`。但 QEMU 最小系统的用户态不建 CPU cgroup，也不开 autogroup 之外的任务组，所以启动路径仍碰不到组实体改权重。另外 `features.h` 的 `HRTICK` 默认 `false`，两个平台上 hrtick 默认都不开火，换出时机默认由周期 tick 里的 `update_deadline()` 决定。第 4.1 节“RK3568 主要走 4.2”的判断据此也不成立。

---

## 1. 两份树差在哪里

| 位置 | 内容 |
|---|---|
| `repo/`（已提交） | EEVDF 主体 + `qos_to_slice()`，`place_entity` / `update_deadline` 直接调用它 |
| `OHOS501/`（工作区） | 同上，另加运行时开关 `sysctl_sched_qos_slice` 和 `sched_entity_slice()` |
| 只在 `OHOS501/` | `kernel/sched/qos_slice_bench.c`，`kernel/sched/rtg/rtg.c` 的 `CONFIG_RTG_LATENCY_FUSION`，`kernel/sched/rtg/Kconfig` |

（审查时的状态。现在两份树一致，`qos_slice_bench.c` 也进了 `repo/`，RTG 那段已删。）

`OHOS501` 的 `fair.c` 相对 `repo` 只多了这个开关。下面第 2–4 节两边都有。第 5 节只在 `OHOS501`。

行号以 `OHOS501/kernel/linux/linux-5.10/kernel/sched/fair.c` 为准。

---

## 2. 和 6.6 对齐、可以保留的部分

这些函数和 6.6 同名实现一致，不要整段重写：

- 数据结构：`sched_entity` 的 `deadline` / `min_deadline` / `vlag` / `slice`，`cfs_rq` 的 `avg_vruntime` / `avg_load`
- 红黑树仍按 `vruntime` 排序，`min_deadline` 做增强值。`__pick_eevdf()` 的走法依赖这个顺序
- `avg_vruntime_add/sub/update`、`entity_eligible`、`update_entity_lag`、`place_entity` 的 lag 补偿
- `update_curr()` 里调用 `update_deadline()`
- `set_next_entity()` 把 `deadline` 暂存进 `vlag`，配合 `RUN_TO_PARITY`
- `enqueue_entity()` / `dequeue_entity()` 去掉了睡眠时的 `vruntime ± min_vruntime`，改由 `place_entity()` 和 `update_entity_lag()` 负责
- `check_preempt_wakeup()` 用 `pick_eevdf()` 判断要不要抢占
- `ENQUEUE_INITIAL` 定义为 `0x80`，和 5.10 已有的 `ENQUEUE_*`（最高用到 `0x40`）不冲突
- `init_task_qos()` 把 `in_qos` 设为 `NO_QOS`（-1）。未设置 QoS 的任务走 `sysctl_sched_min_granularity`，不会误用等级 0
- `qos_apply()` 先写 `in_qos`，再 `sched_set_task_qos_attr()`。随后的出队/入队会在 `place_entity()` 里读到新等级

`pick_eevdf()` 选不中时打 `pr_err("EEVDF scheduling fail, picking leftmost\n")` 是 6.6 原有行为。如果改权重之后这行开始刷屏，先看第 3 节，不要先改挑选函数。

---

## 3. 必须修：`reweight_entity()` 没有接上 EEVDF

5.10 仍是 CFS 写法，位置 `fair.c:3398`。6.6 在 `fair.c:3666` 起有 `reweight_eevdf()`，并在 `reweight_entity()` 里：

- 实体在队列上、又不是 `curr`：先 `__dequeue_entity()`，改完再 `__enqueue_entity()`
- 实体在队列上：按旧权重/新权重重算 `vruntime` 和 `deadline`，保持 lag
- 实体不在队列上：`vlag = vlag * old_weight / new_weight`

5.10 只改 `se->load` 和 PELT，不碰 `avg_vruntime`、`avg_load`、`vruntime`、`deadline`、`vlag`。

分两条路径，严重程度不同。

### 3.1 改 nice / QoS：lag 缩放丢了，平均虚拟时间不会漂

`set_user_nice()`（`core.c:5219`）和 `__sched_setscheduler()`（`core.c:5654`）都是先 `dequeue_task()`，再 `set_load_weight()` → `reweight_task()`。出队时 `update_entity_lag()` 用的还是旧权重，`__dequeue_entity()` 也按旧权重减 `avg_vruntime`。所以这条路径不会把平均值减错。

出队之后 `se->on_rq == 0`。6.6 会在这里缩放 `vlag`，5.10 不会。接着入队，`place_entity()` 用未缩放的 `vlag` 重放。nice 或 QoS 一变，任务欠的/超的服务量就和权重对不上。`deadline` 会在这次 `place_entity()` 里重算，这一项本身不是问题。

### 3.2 组调度：实体还在树上就改权重，平均值会一直漂

`CONFIG_FAIR_GROUP_SCHED=y` 时（RK3568 defconfig 已打开，QEMU defconfig 没有），`update_cfs_group()`（`fair.c:3552`）直接调用 `reweight_entity()`，没有先出队。

触发点：

- `enqueue_task_fair()` / `dequeue_task_fair()` 走到已经 `on_rq` 的父组实体（`fair.c:6117` 和 `6228` 附近的第二段循环）
- `sched_group_set_shares()`（`fair.c:12180`）

组实体在父队列的红黑树上、又不是 `curr` 时：入队按旧权重加进 `avg_vruntime` / `avg_load`，出队按新权重减。两个和不闭合，`entity_eligible()` 会判错，`pick_eevdf()` 可能退回最左节点。

QEMU 没开 cgroup CPU 控制器，所以启动测试碰不到这条。RK3568 开了，任务进出 cgroup 就会碰到。

### 建议改法

把 6.6 的 `reweight_eevdf()` 和 `reweight_entity()` 的出队/入队骨架搬过来。不要只在 `set_user_nice()` 上打补丁，组调度那条不经过它。

---

## 4. 必须修：墙上时间片仍是 CFS，QoS slice 决定不了换出时机

`se->slice` 只进入 `deadline = vruntime + calc_delta_fair(slice, se)`。真正决定“现在换不换下 CPU”的还有两条旧路径。

### 4.1 `check_preempt_tick()`

`entity_tick()`（`fair.c:5105`）在 `nr_running > 1` 时仍调用它（`fair.c:4949`）。比较的是 `sched_slice()`（按权重分的 CFS 理想运行时间）和 `sum_exec_runtime` 的差值，以及和最左实体的 `vruntime` 差。6.6 的 `entity_tick()` 已经删掉这次调用，到期只靠 `update_deadline()`。

`sched_slice()` 在 nice 0、少量任务时大约是数毫秒，高 nice（低权重）更短。后台 14ms 的 slice 会先被这次 CFS 抢占截断。

`CONFIG_SCHED_HRTICK=y` 且 hrtick 定时器在跑时，`entity_tick()` 会提前 return，这条 tick 检查被跳过。RK3568 打开了 HRTICK，所以板上主要走 4.2，不是这条。QEMU 没开 HRTICK，QEMU 上就是这条在截断。

### 4.2 `hrtick_start_fair()`

`fair.c:5959`。定时器长度仍是 `sched_slice()`，不是 `se->slice`，也不是“距离 deadline 还剩多少墙钟时间”。RK3568：`CONFIG_SCHED_HRTICK=y`，`CONFIG_HZ=300`。

高权重任务的 CFS slice 通常长于 1ms，交互线程的 1ms deadline 要等下一次 `update_curr()` 才被看到，hrtick 不会在 1ms 开火。低权重任务的 CFS slice 短于 14ms，后台线程的长 slice 到不了。

### 建议改法

- 去掉 `entity_tick()` 里对 `check_preempt_tick()` 的调用，与 6.6 一致
- `hrtick_start_fair()` 改为按 `se->slice` 已消耗的墙钟时间开火（`sum_exec_runtime - prev_sum_exec_runtime` 对 `se->slice`）。EEVDF 里墙钟跑满一个 slice 才越过 deadline，因为 `vruntime` 和 `calc_delta_fair(slice)` 用的是同一个权重

在这之前，不要用 cyclictest 或切片对照实验下“QoS slice 已改变调度延迟”的结论。选任务顺序可能已经按 deadline 分了，换下 CPU 的时刻仍是 CFS。

---

## 5. 必须修：`sched_yield()` 在 slice 内是空操作

`yield_task_fair()`（`fair.c:7863`）仍调用 `set_skip_buddy()`。`pick_next_entity()`（`fair.c:5036`）只看 `NEXT_BUDDY`（`features.h` 里是 `false`）和 `pick_eevdf()`，没有任何地方再读 `cfs_rq->skip`。

`RUN_TO_PARITY` 为 true。`set_next_entity()` 把 `vlag` 设成当时的 `deadline`。slice 还没过期时，`__pick_eevdf()` 直接返回当前任务。`schedule()` 会再选中它。

6.6 的 `yield_task_fair()` 在 `update_curr()` 之后做：

```c
se->deadline += calc_delta_fair(se->slice, se);
```

这样当前任务不再是最早 deadline，`RUN_TO_PARITY` 的 `vlag == deadline` 也不再成立。

影响 `sched_yield()` 和走到 `yield_task` 的 `cond_resched()`。队列里只有它自己时两边都直接 return，没有差别。

---

## 6. 不要合：`CONFIG_RTG_LATENCY_FUSION`

只在 `OHOS501`，Kconfig 默认 `n`，RK3568 defconfig 没有打开它。打开就会坏。

1. `kernel/sched/core.c:882` 的 `set_latency_weight()` 是 `static`。`rtg.c:299` 从另一个编译单元调用，链接失败。
2. 就算改成全局符号，`latency_nice` 也不写入 `se->slice` / `deadline`。`check_preempt_wakeup()` 已经改成 `pick_eevdf()`，不再调用 `wakeup_preempt_entity()`。Kconfig 里“让 EEVDF 通过 deadline 感知 QoS”不成立。`08-QEMU可行性分析.md` 已有同样结论。
3. `rtg_latency_map[]` 用的是 RTG 的 `grp->id`（coloc 组号），不是 `QosLevel`。组号 4 会被写成 `latency_nice = -20`。`NICE_TO_LATENCY(-20)` 的下标是 0，不会越界，但语义是错的。

不要为了编过把 `set_latency_weight()` 改成非 static 然后打开这个开关。要感知 QoS，只走已经接上的 `in_qos → qos_to_slice()`。

---

## 7. 策略问题：`QOS_KEY_BACKGROUND` 的 slice

`fair.c:598` 的表：

| in_qos | 等级 | slice |
|---|---|---|
| 0 | `QOS_BACKGROUND` | 14ms |
| 1 | `QOS_UTILITY` | 10ms |
| 2 | `QOS_DEFAULT` | 4ms |
| 3 | `QOS_USER_INITIATED` | 2ms |
| 4 | `QOS_DEADLINE_REQUEST` | 1.5ms |
| 5 | `QOS_USER_INTERACTIVE` | 1ms |
| 6 | `QOS_KEY_BACKGROUND` | 14ms |

用户态 `QosLevel` 从 0 递增，`qos_interface.cpp` 把 `level` 原样传进 ioctl，内核 `qos_apply()` 把同一个数写入 `in_qos`。0–5 越高等级 slice 越短。6 突然回到和后台一样。

`include/linux/sched.h` 里写的是 `in_qos` 越大 QoS 越高。按这句话，等级 6 不该是最长 slice。若产品语义就是“关键后台，可以让前台”，应在注释里写明，不要让表看起来像笔误。

公开 NDK 只允许到 `QOS_USER_INTERACTIVE`。等级 6 来自内部 `SetThreadQos`。

---

## 8. 残留的 CFS 虚拟时间归一化（默认配置下不会单独放错任务）

6.6 已经去掉这些 `vruntime ± min_vruntime`。5.10 还留着，但入队总会再调 `place_entity()`，放置用的是 `vlag` 不是当前 `vruntime`，所以默认路径会被盖掉。

| 位置 | 行为 | 默认配置下的结果 |
|---|---|---|
| `task_fork_fair()` `fair.c:11768` | `place_entity(se, 1)`，然后 `vruntime -= min_vruntime` | `1` 不是 `ENQUEUE_INITIAL`（0x80），fork 时不会给半个 slice。第一次入队会再 `place_entity()`，这次也不带 `ENQUEUE_INITIAL`。本树的 6.6 同样只在 fork 时给半片、入队时冲掉，和上游这个版本没有用户可见差别 |
| `sysctl_sched_child_runs_first` | 交换父子 `vruntime` 后再只减子任务的 `min_vruntime` | 默认 0，不走。设成 1 时，子任务入队重放会盖掉交换结果，父任务的 `vruntime` 留在被交换后的值 |
| `migrate_task_rq_fair()` `fair.c:7488` | `TASK_WAKING` 时 `vruntime -= min_vruntime`，注释仍说入队会加回来 | 入队不会加回来，而是 `place_entity()` 重写。减法是死代码 |
| `detach_task_cfs_rq()` / `attach_task_cfs_rq()` `fair.c:11902` | 睡眠任务先 `place_entity()`，再减/加 `min_vruntime`。`deadline` 不跟着平移 | 下次唤醒的 `place_entity()` 会按 `vlag` 重写 `vruntime` 和 `deadline` |

这组是清理项，排在第 3–5 节后面。不要为了“和注释一致”在入队里补回 `vruntime += min_vruntime`，那会和 `place_entity()` 叠成两次放置。

---

## 9. 建议的修改顺序

1. 从 6.6 搬 `reweight_eevdf()`，并让 `reweight_entity()` 在树上先摘后挂。用 cgroup 里两个不同权重的任务反复进出，确认 `avg_load` 不会在空队列上变成非 0。
2. 去掉 `check_preempt_tick()` 的调用；`hrtick_start_fair()` 改用 `se->slice`。然后才能做 QoS slice 的对照。
3. `yield_task_fair()` 改为把 `deadline` 往后推一个 slice。两个忙等线程，其中一个 `sched_yield()`，应让出 CPU。
4. 保持 `CONFIG_RTG_LATENCY_FUSION` 关闭。不把它写进 defconfig。
5. 定 `QOS_KEY_BACKGROUND` 的 slice，并改注释。
6. 有余力再删第 8 节的 `min_vruntime` 加减，使 fork/migrate 与 6.6 相同。

验证时至少覆盖：改 nice 后再看 lag、cgroup 组权重变化后空队列的 `avg_vruntime`/`avg_load`、`sched_yield()`、以及 slice 开关打开前后的换出间隔。只看 QEMU 能否启动不够。

---

## 10. 2026-09-28 已落地的修改

| 项 | 改法 |
|---|---|
| 第 3 节 | 增加 `reweight_eevdf()`。在树上的实体先 `__dequeue_entity()`，改完再 `__enqueue_entity()`。不在队列上的实体按 `old_weight / new_weight` 缩放 `vlag` |
| 第 4 节 | 删除 `check_preempt_tick()`。`hrtick_start_fair()` 用 `se->slice`，不再用 `sched_slice()` |
| 第 5 节 | `yield_task_fair()` 把 `deadline` 往后推一个 `calc_delta_fair(se->slice)`。删掉已无调用者的 `set_skip_buddy()` |
| 第 6 节 | 本地 `OHOS501` 去掉 `CONFIG_RTG_LATENCY_FUSION`。远程快照里没有这段代码 |
| 第 7 节 | `QOS_KEY_BACKGROUND` 定为 4ms，和 `QOS_DEFAULT` 相同，理由见下 |
| 第 8 节 | 按 6.6 删掉 `task_fork_fair()`、`migrate_task_rq_fair()`、`detach/attach_task_cfs_rq()` 里的 `min_vruntime` 加减。`task_fork_fair()` 改为 `place_entity(cfs_rq, se, ENQUEUE_INITIAL)`，不再处理 `sysctl_sched_child_runs_first` 的交换 |

第 7 节：当天先改成过 0.5ms，复查时撤回。`qos_manager` 的每一套策略都给 `QOS_KEY_BACKGROUND` nice 0、不走 RT，与 `QOS_DEFAULT` 完全相同，没有任何地方把它当作比交互更急的等级。给它全表最短的 slice 会让它比前台交互线程更频繁地抢占，产品语义里找不到依据。表里的注释已写明这一点。

不要在入队路径补回 `vruntime += min_vruntime`。

## 11. 第二轮复查补的修复

第 10 节落地后又对照 6.6 查了一遍，找到两处会放错任务的问题，以及一批死代码。

| 问题 | 原因 | 改法 |
|---|---|---|
| fork 出的子任务落后 V 很多 | `set_next_entity()` 为 `RUN_TO_PARITY` 把 `deadline` 暂存进 `vlag`。`dup_task_struct()` 把正在运行的父任务整个拷过去，子任务的 `vlag` 就是这个暂存值。`wake_up_new_task()` 里的 `place_entity()` 把它当作 lag，子任务一出生就落后 V 大约一个 `deadline` 的量 | `__sched_fork()` 清零 `se.vlag`，并把 `se.slice` 初始化为 `sysctl_sched_min_granularity`，与 6.6 相同 |
| 在树上改权重时 V 被挪动 | 第 10 节的 `reweight_entity()` 先 `__dequeue_entity()`，再在 `reweight_eevdf()` 里算 `avg_vruntime()`，这时的 V 不含该实体。6.6 推论 2 “保持 lag 则 V 不变”要求 V 包含它 | 在摘下之前算 V，传给 `reweight_eevdf(se, avruntime, weight)`。`update_min_vruntime()` 放到实体挂回之后 |
| `-Wunused-function` | EEVDF 之后 `__pick_next_entity`、`__sched_period`、`sched_slice`、`sched_vslice`、`set_last_buddy`、`vruntime_normalized` 已无调用者；`wakeup_gran`、`wakeup_preempt_entity` 只剩 `CONFIG_SCHED_LATENCY_NICE` 下的调用 | 删除前六个，后两个收进 `CONFIG_SCHED_LATENCY_NICE` |
| `hrtick_start_fair()` / `yield_task_fair()` 的兜底 | `se->slice` 在 fork 时已经初始化，不会为 0 | 去掉兜底，与 6.6 相同 |
| `get_rr_interval_fair()` | 仍用 `sched_slice()` | 改为 `NS_TO_JIFFIES(se->slice)` |

两处放错任务的问题都在 QEMU 上验证过：修复后的内核通过，只回退这两处的反证内核失败，数据见 `09-QEMU对照结果.md` 第 3、4 节。
