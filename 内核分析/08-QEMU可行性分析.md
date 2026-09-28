# QEMU 平台可行性分析

> 日期: 2026-09-28
> 范围: OpenHarmony 5.0.1-Release / Linux 5.10.208 调度器修改
> 验证平台: 只使用 QEMU `qemu-arm64-linux-min`，不再使用 DAYU200 / RK3568
> 结论依据: `内核分析/` 文档、`进度文件夹/` 构建记录，以及 `OHOS501/kernel/linux/linux-5.10` 当前未提交改动

---

## 1. 平台决定

验证只在 QEMU 上做。启动参数来自 `OHOS501/vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh`:

```
qemu-system-aarch64 -M virt -smp 4 -m 1024 -nographic -cpu cortex-a57
```

rk3568 全量编译（2026-08-02）和 `prebuilt_dayu200` 官方镜像只保留为历史记录，不再作为交付物。EEVDF 参考树仍是同仓库的 `linux-6.6`（6.6.22）。这里改的是标准 Linux 调度器，不是鸿蒙微内核。

## 2. 已经落地的部分

- QEMU 基线已跑通。2026-08-12 `qemu-arm64-linux-min` 启动到 samgr ready。2026-08-21 换上 EEVDF 内核后再次启动 120 秒，无致命崩溃。产物在 `OHOS501/out/qemu-arm-linux`。之后只需要重编这套 QEMU 内核，不必再做 rk3568 全量编译。
- EEVDF 已写进 5.10，相对 `OpenHarmony-5.0.1-Release` 尚未提交。主体在 `kernel/sched/fair.c`：`avg_vruntime`、`entity_eligible`、`pick_eevdf`、`place_entity`、`update_deadline`。配套改动在 `kernel/sched/sched.h`、`kernel/sched/features.h`、`include/linux/sched.h`、`include/linux/rbtree_augmented.h`。
- QoS→slice 已接到选任务路径。`qos_to_slice()` 用 `task->qts.in_qos` 决定 `se->slice`，再决定 virtual deadline。QEMU defconfig 中 `CONFIG_QOS_CTRL=y`。这是代码里真实存在的联动。
- 2026-08-28 另加了 `kernel/sched/rtg/rtg.c` 的 `CONFIG_RTG_LATENCY_FUSION`（Kconfig 默认 `n`，defconfig 未打开）。入组时按 RTG group id 写 `latency_nice`。

还没有量化对照。`EEVDF移植修改记录.md` 与 `EEVDF移植量化测试方案.md` 里的四组表格是空的。`02-内核优化文件地图.md` 里的内存碎片主题没有实现，QEMU 范围内不再做。

## 3. QEMU 最小系统的限制

`vendor/ohemu/qemu_arm64_linux_min/config.json` 的 `resourceschedule` 只包含 `ffrt`，没有 `qos_manager`。用户态 `SetThreadQos` 不在这套镜像里。

因此测试程序必须在客户机里直接设置内核 QoS（改 `task->qts.in_qos`），不能依赖开发板上的 QoS 服务。

数据通路如下：

```
测试程序设置 in_qos
        |
        v
qos_to_slice()  -->  se->slice  -->  EEVDF deadline  -->  pick_eevdf()

RTG 入组 --> latency_nice / latency_weight --> 仅唤醒抢占阈值
                                              （不进入 deadline）
```

## 4. 结论

**修改后的内核在 QEMU 上启动：可行，而且已经做到。** 去掉开发板之后，工程路径更短。

**按 `EEVDF移植量化测试方案.md` 证明「RTG 入组设置 latency_nice，从而使 EEVDF deadline 感知 QoS」：不可行。** 这条因果链在源码里是断的，换平台补不上。

**在 QEMU 上证明 QoS 等级通过 slice 改变「谁先被选中」：可行，指标必须改。** 能测的是同一 vCPU 上的调度顺序、客户机内等待时间的相对差、公平份额。不能测、也不应写入报告的是：实机调度延迟的绝对微秒数、音频 underrun、帧时间、fps。当前环境是 x86 主机上的 aarch64 TCG，墙钟时间主要是仿真开销，`cyclictest` 的绝对数值没有意义。

## 5. 现有竞赛叙事为什么站不住

### 5.1 latency_nice 没有接到 EEVDF

`set_latency_weight()` 只更新 `se->latency_weight`。5.10 树里读取该字段的位置是原有的 `wakeup_latency_gran()` 和 `check_preempt_from_idle()`，也就是 OHOS 已有的唤醒抢占偏置。

`place_entity()` 和 `update_deadline()` 不读 `latency_nice`。linux-6.6 的 `fair.c` 里也没有 `latency_nice`。测试方案里「C 组与 B 组只差一行 `set_latency_weight`，直接证明对 deadline 的贡献」测到的不是 deadline。

### 5.2 RTG group id 不是 QoS 等级

`rtg.c` 的映射表把 group 1–4 注释成 `QOS_DEFAULT` 到 `QOS_USER_INTERACTIVE`。RTG 的 id 是关联线程组编号，不是 `in_qos` 枚举。即使打开 `CONFIG_RTG_LATENCY_FUSION`，语义也对不上。

### 5.3 对照开关不存在

量化方案依赖 `CONFIG_SCHED_EEVDF`。linux-5.10 树里没有这个符号。EEVDF 是直接改写 `fair.c`，不能用 Kconfig 在 CFS 和 EEVDF 之间切换。CFS 对照只能另编一版未改 `fair.c` 的 QEMU 内核。

### 5.4 文档之间不一致

- `06-EEVDF与QoS适配记录.md`（2026-08-21）描述的是 `qos_to_slice`，与当前 `fair.c` 一致。
- `EEVDF移植修改记录.md`（2026-08-28）和 `EEVDF移植量化测试方案.md` 改口成 latency_nice，并写 fair.c「无修改」。
- 量化方案的软件基线写成 OpenHarmony 6.1。实际工程是 5.0.1，验证面是 QEMU。

答辩应以 `qos_to_slice` 为准。`CONFIG_RTG_LATENCY_FUSION` 退出创新点。

### 5.5 原测试方案的环境是板级的

锁 cpufreq、关 IRQ 均衡、音频和帧率场景在 `virt` 机器上不存在。不能把文档里的板名换成 QEMU 就继续用那份方案。

## 6. QEMU 上仍然存在的正确性缺口

> 2026-09-28 状态：下面三条都已修复。迁移和 fork 路径的 `min_vruntime` 加减已删，`reweight_entity()` 已接上 `reweight_eevdf()`，fork 时子任务不再继承父任务暂存的 deadline。见 `09-EEVDF审查交接.md` 第 10、11 节，QEMU 验证见 `09-QEMU对照结果.md`。下文保留当时的分析。

`-smp 4` 仍会做负载均衡，所以只在 QEMU 上跑并不能躲开 SMP 问题。和本仓库 6.6 对照：

- **迁移仍是 CFS 的 vruntime 归一化。** `fair.c` 约 7481、11772、11901 行还有 `se->vruntime -= min_vruntime`。6.6 已改为用 lag 携带跨 CPU 状态。4 个 vCPU 互相迁移时，virtual time 会被打乱。
- **`reweight_entity()` 没有 EEVDF 化。** 6.6 在改权重时调用 `reweight_eevdf()` 并缩放 `vlag`，同时把实体摘下再插回增强树。5.10 这段仍只改 `load.weight`。nice 或 cgroup 改权重后，`avg_vruntime` 和树上的 key 会不一致。
- **`set_next_entity()` 把 deadline 暂存进 `vlag`。** 出队路径如果没有调用 `update_entity_lag()`，下一次 `place_entity()` 会把 deadline 当成 lag。`pick_eevdf()` 失败会 `pr_err` 并退回最左节点。

主实验应把负载绑到 **1 个 vCPU**，这样归因不经过迁移。4 vCPU 只作为「迁移修完之后系统仍能工作」的附加项，不用来证明 slice 的收益。单 vCPU 的 slice 对照可以先做，不必等迁移和 `reweight_entity` 补完。

## 7. 若继续，QEMU 上可交付的命题

命题改为 `06-EEVDF与QoS适配记录.md` 已经实现的那条，并限定测量范围：

> 在 QEMU 的 EEVDF 上，测试程序直接设置 `in_qos`。短 slice 的任务更早被 `pick_eevdf` 选中。用 sched trace 的选中顺序和客户机内等待时间的相对差来证明。对照是同一套 QEMU 用户态下的两版内核：未改 slice 的 EEVDF，以及原版 CFS。

要做到这一步，最少包括：

- 只编、只跑 `qemu-arm64-linux-min`。不再编 rk3568，不再烧录。
- 测试程序在客户机里直接设置内核 QoS，不依赖 `qos_manager`。
- CFS 对照用单独的内核构建（git 分支或补丁）。slice 映射单独做成开关。不要使用文档里的 `CONFIG_SCHED_EEVDF`。
- 主实验用 `taskset` 绑到一个 vCPU，用 `ftrace` / `sched_switch` 看选中顺序。报告写明 TCG。不报绝对微秒延迟，不报帧率和音频。
- 预期收束为：高 QoS 更频繁地被重新调度，等待顺序更靠前。吞吐和上下文切换可能变差。不要写「实机显著降低调度延迟」。

## 8. 总体判断

| 目标 | 判断 |
|---|---|
| 修改后的内核在 QEMU 启动 | 可行，已完成 |
| 不再做 DAYU200 / rk3568 | 与当前代码和已有 QEMU 产物一致 |
| 按现稿证明 latency_nice 融合改变 EEVDF deadline | 不可行，与是否上板无关 |
| 在 QEMU 单 vCPU 上证明 QoS→slice 改变选中顺序 | 可行。需要对照内核、直接设置 `in_qos`，并且不把 TCG 墙钟当成实机延迟 |
| 用 QEMU 代替原方案里的 cyclictest 绝对延迟、音频、帧率 | 不可行。这些指标在该平台上没有意义 |
| 同时做内存碎片优化 | 不做。没有实现，也不在 QEMU 验证范围内 |
