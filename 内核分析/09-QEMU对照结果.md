# QEMU 单 vCPU 的 QoS→slice 对照结果

> 日期: 2026-09-28
> 平台: `qemu-arm64-linux-min`，`qemu-system-aarch64 -M virt -smp 4 -m 1024 -cpu cortex-a57`
> 内核: Linux 5.10.208，EEVDF 移植 + `qos_to_slice()`
> 结论: 通过。短 slice 的任务被 `pick_eevdf` 选中的次数约为长 slice 的 4.25 倍，两边 CPU 时间仍接近 1:1。

## 1. 对照怎么做

没有另编一版 CFS 内核，也没有再编 rk3568。同一颗 EEVDF 内核里用 `/proc/sys/kernel/sched_qos_slice` 开关 slice 映射：

- `0`: 两个任务都使用 `sysctl_sched_min_granularity`（这次实测 2.25ms）
- `1`: `in_qos` 决定 slice。交互任务 1ms，后台任务 14ms

两个内核线程绑在 CPU 0，nice 相同。`in_qos` 在第一次入队前直接写入 `task->qts.in_qos`，不经过 `qos_manager`，也不改 `latency_nice`。窗口 400ms。计数的是 `pick_next_entity()` 选中次数，不是循环次数。

启动参数 `eevdf_qos_bench=1` 会在 `late_initcall` 里自动跑这两段并打到串口。复现：

```bash
cd ~/2024204088/OHOS501
EEVDF_QOS_BENCH=1 ./vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh \
  -e ~/2024204088/qemu-bench-imgs
```

镜像目录里的 `Image` 是这次编出来的内核。用户态镜像仍是 2026-08-21 的 QEMU 最小系统。编译产物在 `OHOS501/out/qos-bench-obj`（约 602M），没有动 `out/kernel` 里那份 root 拥有的旧树。

## 2. 实测

串口时间是客户机时钟。墙钟主要是 TCG 仿真，下面不把这些纳秒当成实机延迟。

关闭 slice 映射（两边 slice 都是 2.25ms）：

| 任务 | in_qos | se.slice | 选中次数 | CPU 时间 |
|---|---|---|---|---|
| hi | 5 | 2.25ms | 51 | 203.4ms |
| lo | 0 | 2.25ms | 51 | 205.3ms |

`pick_ratio_x100=100`，`exec_ratio_x100=99`。选中次数和 CPU 时间都接近 1:1。

打开 slice 映射（hi 1ms，lo 14ms）：

| 任务 | in_qos | se.slice | 选中次数 | CPU 时间 |
|---|---|---|---|---|
| hi | 5 | 1ms | 51 | 201.2ms |
| lo | 0 | 14ms | 12 | 204.3ms |

`pick_ratio_x100=425`，`exec_ratio_x100=98`。内核判定 `verdict=pass`。随后启动继续走到 `SAMGR: set samgr ready ret : succeed`。

## 3. 这些数字说明什么

- slice 映射确实写进了调度实体：关闭时两边都是 2.25ms，打开时分别是 1ms 和 14ms。
- 打开之后，短 slice 的任务被选中的次数是长 slice 的 4.25 倍。
- CPU 时间仍接近平分。变的是轮转粒度，不是权重份额。
- 倍数不是 14，因为 QEMU 这颗内核 `HZ=250`，周期 tick 是 4ms。1ms 的请求最快也要到下一次 tick 才被看见，所以实际比接近 14ms/4ms。这是仿真和 tick 的限制，不是实机微秒延迟。

为了让 slice 真的影响抢占，补了两处原来仍走 CFS 时间片的路径：`hrtick_start_fair()` 和 `check_preempt_tick()`。开关关闭时它们仍用原来的 `sched_slice()`。

## 4. 没有做的事

- 没有把 `CONFIG_RTG_LATENCY_FUSION` 当作结果。它默认关闭，而且不改 deadline。
- 没有报帧率、音频、`cyclictest` 绝对延迟。
- 没有做内存碎片，没有编 rk3568。
- `drivers/auth_ctl` 在只打开 `CONFIG_QOS_CTRL` 时编不过（空 enum，以及 `i` 的声明被错误地包在 `CONFIG_QOS_AUTHORITY` 里）。已改 `common_modules/qos_auth`，否则 QEMU 配置里的 QoS 进不了内核。
