# QEMU 上的 QoS→slice 对照和 EEVDF 不变量检查

> 日期: 2026-09-28
> 平台: `qemu-arm64-linux-min`，`qemu-system-aarch64 -M virt -smp 4 -m 1024 -cpu cortex-a57`
> 内核: Linux 5.10.208 `#10 SMP PREEMPT Mon Sep 28 11:56:54 CST 2026`，源码即 `repo/` 快照
> 结论: 全部通过。短 slice 的任务被 `pick_eevdf` 选中的次数约为长 slice 的 3.9 倍，CPU 时间仍接近 1:1。fork、组权重变化、平均虚拟时间三项检查在修复后通过，在只回退修复的反证内核上失败。

同一天较早的一版数据（4.25 倍）来自修复 fork 和 `reweight_entity()` 之前的中间内核，已被本文替换。

## 1. 怎么跑

没有另编一版 CFS 内核，也没有再编 rk3568。启动参数 `eevdf_qos_bench=1` 会在 `late_initcall` 里依次跑 QoS 对照、fork 检查、改权重检查，并在后台跑 60 秒平均值扫描，结果打到串口。不带这个参数时这些都不跑，`fair.c` 里的统计钩子也不记录。

```bash
cd ~/2024204088/OHOS501
EEVDF_QOS_BENCH=1 ./vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh \
  -e ~/2024204088/qemu-bench-imgs
```

`qemu-bench-imgs/Image` 是 #10 内核。用户态镜像仍是 2026-08-21 的 QEMU 最小系统。编译目录是 `OHOS501/out/qos-bench-obj`，源码从 `OHOS501/out/KERNEL_OBJ/kernel/src_tmp/linux-5.10` 编，调度器文件与 `OHOS501/kernel/linux/linux-5.10` 逐字节一致。

这颗内核的相关配置：

| 配置 | 值 | 来源 |
|---|---|---|
| `CONFIG_HZ` | 250 | defconfig |
| `CONFIG_SCHED_AUTOGROUP` | y | defconfig，连带选中 `CGROUP_SCHED` / `FAIR_GROUP_SCHED` |
| `CONFIG_SCHED_HRTICK` | y | 但 `features.h` 的 `HRTICK` 默认 `false`，hrtick 不开火 |
| `CONFIG_QOS_CTRL` / `CONFIG_AUTHORITY_CTRL` | y | 本项目加入 defconfig |

串口时间是客户机时钟，墙钟主要花在 TCG 仿真上，下面的纳秒不当作实机延迟。

## 2. QoS→slice 对照

两个内核线程绑在 CPU 0，nice 相同。`in_qos` 在第一次入队前直接写入 `task->qts.in_qos`，不经过 `qos_manager`，也不改 `latency_nice`。每段 400ms。计数的是 `pick_next_entity()` 的选中次数。`/proc/sys/kernel/sched_qos_slice` 切换两段：

- `0`: 两个任务都用 `sysctl_sched_min_granularity`，实测 2.25ms
- `1`: `in_qos` 决定 slice。交互任务（5）1ms，后台任务（0）14ms

| 段 | 任务 | in_qos | se.slice | 选中次数 | CPU 时间 |
|---|---|---|---|---|---|
| 关闭 | hi | 5 | 2.25ms | 52 | 206.9ms |
| 关闭 | lo | 0 | 2.25ms | 52 | 208.6ms |
| 打开 | hi | 5 | 1ms | 51 | 194.8ms |
| 打开 | lo | 0 | 14ms | 13 | 200.8ms |

关闭时 `pick_ratio_x100=100`、`exec_ratio_x100=99`。打开时 `pick_ratio_x100=392`、`exec_ratio_x100=97`，`verdict=pass`。

- slice 映射确实写进了调度实体，`se_slice_ns` 分别是 2250000 / 1000000 / 14000000。
- 打开后短 slice 的任务被选中的次数约是长 slice 的 3.9 倍，CPU 时间仍接近平分。变的是轮转粒度，不是权重份额。
- 倍数不是 14。`HZ=250` 时周期 tick 是 4ms，hrtick 又没开，1ms 的请求最早在下一次 tick 才被看见，实际比例接近 14ms/4ms。同一内核多次启动在 3.9–4.3 之间。

开关关闭时 `hrtick_start_fair()` 和 `yield_task_fair()` 也都用 `se->slice`，只是这时它等于 `sysctl_sched_min_granularity`。5.10 原来的 `sched_slice()` 已经删掉。

## 3. EEVDF 不变量检查

| 检查 | 做法 | 通过条件 | #10 结果 |
|---|---|---|---|
| `EEVDF_FORK_CHECK` | CPU 0 上一个陪跑线程，父内核线程忙等 1ms 后 `kernel_thread()` 生子，重复 16 次。子线程第一次运行时在 rq 锁下读 `V - v` | 父线程的 `vlag` 确实是暂存的 deadline（≥100ms），且子线程 lag 都 ≤20ms | 16/16 暂存，父线程最小暂存 0.56s，子线程最大 lag 2.7ms，pass |
| `EEVDF_REWEIGHT_CHECK` | CPU 0 上两个陪跑线程各自新建 autogroup，200 轮交替 `sched_group_set_shares()` 128↔2048。每次改到仍在树上的组实体时比较改前改后的 V | `|V' - V|` 不超过按本次权重算出的截断误差上界（几 ns），且每轮全量扫描一致 | 191 次在树上改权重，0 次越界，最大偏移 8ns，pass |
| `EEVDF_AVG_CHECK` | 启动后 60 秒，每 20ms 在 rq 锁下扫描所有 CPU 的根 cfs_rq 和叶子 cfs_rq，用红黑树里的实体重算 `avg_vruntime`、`avg_load`、每个节点的 `min_deadline` | 三项与增量维护的值完全相等 | 2135 次扫描、8573 个队列、259 个实体（其中组实体 15 个），不一致 0 次，pass |

启动继续走到 `SAMGR: set samgr ready ret : succeed`，串口没有 `BUG:` / `WARNING:` / `Oops` / RCU stall。不带 `eevdf_qos_bench=1` 再启动一次，串口没有任何 `EEVDF_` 输出，samgr 同样就绪。

改权重检查的上界来自 `scale_load_down()` 截断：`avg_vruntime` 按截断后的权重求和，`reweight_eevdf()` 按完整权重缩放 lag，两边差出的只有取整误差。8ns 在上界之内。

## 4. 反证：只回退两处修复

为确认这两项检查测得出问题，在编译树里只改了两行，其余不动，编出 #9：

- `reweight_entity()` 改回在摘下实体之后才算 `avg_vruntime()`（即不含该实体的 V）
- `__sched_fork()` 不再清零 `se.vlag`

| 检查 | #9 结果 |
|---|---|
| `EEVDF_FORK_CHECK` | 父线程暂存 0.68s，子线程最大 lag 0.69s，fail。子线程把父线程暂存的 deadline 当成 lag 继承了 |
| `EEVDF_REWEIGHT_CHECK` | 200/200 越界，最大偏移约 3.7×10¹³ns，fail |
| `EEVDF_AVG_CHECK` | 不一致 0 次 |
| QoS 对照 | pass |

改权重那一项偏移这么大，是因为用不含自身的 V 保持 lag，会让组实体的 `vruntime` 带着误差进入下一次 16 倍的权重翻转，误差逐轮放大。累加和本身仍然闭合，所以只做平均值扫描发现不了这个问题。这也是第 3 节要单独检查 V 是否保持的原因。

#9 意外又跑了一次（`cp -a` 保留了旧时间戳，make 没有重编），fork 最大 lag 0.59s、改权重 199/199 越界，结论相同。之后 `touch` 源文件编出 #10，就是第 3 节的结果。

## 5. 没有做的事

- 没有把 `CONFIG_RTG_LATENCY_FUSION` 当作结果。它已从本地树删除，本来也不改 deadline。
- 没有报帧率、音频、`cyclictest` 绝对延迟。
- 没有做内存碎片，没有编 rk3568。
- `drivers/auth_ctl` 在只打开 `CONFIG_QOS_CTRL` 时编不过（空 enum，以及 `i` 的声明被错误地包在 `CONFIG_QOS_AUTHORITY` 里）。已改 `common_modules/qos_auth`，改动在 `repo/kernel/linux/common_modules/`。
