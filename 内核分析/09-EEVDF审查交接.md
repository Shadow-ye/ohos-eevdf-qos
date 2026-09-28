# EEVDF 移植审查交接

> 审查日期：2026-09-28
> 修复日期：2026-09-28。第 3–7 节已改入 `repo/kernel/linux/linux-5.10/kernel/sched/fair.c`，并同步到本地 `OHOS501` 的同名文件。第 8 节的 `min_vruntime` 残留未动。`CONFIG_RTG_LATENCY_FUSION` 已从本地 `OHOS501` 的 `rtg.c` / `Kconfig` 删掉；远程仓库本来就没有这份 RTG 改动。
> 对照源：`OHOS501/kernel/linux/linux-6.6/kernel/sched/fair.c`（本树里的 6.6，不是更后面的主线修复）
> 审查对象：`OHOS501/kernel/linux/linux-5.10`
> 已提交快照：`repo/kernel/linux/linux-5.10`，commit `d3263fb`（`feat: EEVDF scheduler migration`）
> 本次只审查，没有改内核代码
>
> 同日后续：`hrtick_start_fair()` 和 `check_preempt_tick()` 已在 `sched_qos_slice=1` 时改用 `se->slice`。QEMU 单 vCPU 对照已跑通，见 `09-QEMU对照结果.md`。下面第 4 节保留审查当时的问题描述。

120 秒 QEMU 启动通过，不能说明下面的问题不存在。QEMU 的 `qemu-arm-linux_standard_defconfig` 没有打开 `CONFIG_CGROUP_SCHED` / `CONFIG_SCHED_HRTICK`。RK3568 的 `rk3568_standard_defconfig` 两项目前都是 `y`。启动路径几乎不改权重、不调用 `sched_yield()`，也测不到 QoS 切片是否真的决定抢占点。

---

## 1. 两份树差在哪里

| 位置 | 内容 |
|---|---|
| `repo/`（已提交） | EEVDF 主体 + `qos_to_slice()`，`place_entity` / `update_deadline` 直接调用它 |
| `OHOS501/`（工作区） | 同上，另加运行时开关 `sysctl_sched_qos_slice` 和 `sched_entity_slice()` |
| 只在 `OHOS501/` | `kernel/sched/qos_slice_bench.c`，`kernel/sched/rtg/rtg.c` 的 `CONFIG_RTG_LATENCY_FUSION`，`kernel/sched/rtg/Kconfig` |

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
| 第 7 节 | `QOS_KEY_BACKGROUND` 改为 0.5ms，短于 `QOS_USER_INTERACTIVE` 的 1ms，与“`in_qos` 越大越优先”一致 |

第 8 节未改。不要在入队路径补回 `vruntime += min_vruntime`。
