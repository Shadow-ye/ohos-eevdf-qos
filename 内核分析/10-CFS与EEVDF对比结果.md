# QEMU 上原版 CFS 与 EEVDF 移植的对比

> 日期: 2026-09-28
> 平台: `qemu-arm64-linux-min`，`-M virt -smp 4 -cpu cortex-a57`，TCG 仿真，`CONFIG_HZ=250`，`HRTICK` 特性默认关闭
> 内核: 原版 CFS 是 OHOS 5.0.1 内核 git `HEAD`（`8d8762e82`）的调度器文件，编出 `#12`。EEVDF 是 `repo/` 快照的调度器文件，编出 `#11`。两颗内核的其余源码、defconfig、编译器相同
> 结论: 公平份额两边一样准。有忙等任务时，EEVDF 的唤醒延迟明显比 CFS 差，打开 QoS slice 后更差。结论与 `07` 的预期相反，见第 4 节

## 1. 怎么测

测试代码在 `kernel/sched/sched_cmp_bench.c`，两颗内核都能编，启动参数 `sched_cmp_bench=1` 触发，不带参数时不运行。

```bash
cd ~/2024204088/OHOS501
cp ~/2024204088/qemu-bench-imgs/Image-cfs ~/2024204088/qemu-bench-imgs/Image   # 或 Image-eevdf
SCHED_CMP_BENCH=1 ./vendor/ohemu/qemu_arm64_linux_min/qemu_run.sh -e ~/2024204088/qemu-bench-imgs
```

`SCHED_CMP_BENCH=1` 同时加上 `isolcpus=3`。所有被测线程都是绑在 CPU 3 上的内核线程，启动期用户态不会进这个队列。两颗内核交替启动，各 3 次。

| 场景 | 做法 |
|---|---|
| `lat_hN` | 一个周期线程每 10ms 用绝对时间的 hrtimer 睡到点，醒来忙 0.5ms。旁边 N 个 nice 0 的忙等线程。延迟 = 定时器到期到周期线程真正运行的时间，每场景 300 个样本 |
| `lat_hN_qos` | 同上，周期线程 `in_qos=5`（EEVDF 上 slice 1ms），忙等线程 `in_qos=0`（EEVDF 上 slice 14ms）。nice 都是 0，只有 slice 不同。原版 CFS 内核不读 `in_qos`，所以这一行在 CFS 上等于没有 QoS |
| `fair_n0_nX` | 两个忙等线程，nice 0 对 nice X，2 秒，CPU 时间比和权重比的偏差 |
| `pingpong` | 两个线程用 completion 来回交接 20000 次，一次往返的客户机时间，3 次取中位数 |

## 2. 结果

每格是 3 次启动的中位数，单位 ms。

| 场景 | CFS 平均 | CFS p90 | CFS p99 | EEVDF 平均 | EEVDF p90 | EEVDF p99 |
|---|---|---|---|---|---|---|
| `lat_h0`（无竞争，仿真底噪） | 0.23 | 0.32 | 0.94 | 0.21 | 0.34 | 0.56 |
| `lat_h1` | 0.14 | 0.20 | 0.57 | 0.66 | 2.18 | 5.88 |
| `lat_h4` | 0.13 | 0.18 | 0.60 | 2.24 | 4.09 | 8.08 |
| `lat_h1_qos` | 0.13 | 0.20 | 0.49 | 0.80 | 1.31 | 13.54 |
| `lat_h4_qos` | 0.13 | 0.19 | 0.63 | 4.39 | 13.53 | 15.62 |

- EEVDF 的三次启动之间很稳定，同一场景 p99 的差别最多约 1.6ms。CFS 在有竞争的场景里 p99 每次都在 1.2ms 以内。
- 两边各有一次单点尖峰（CFS `lat_h0` 77ms，EEVDF `lat_h0` 36ms），出现在无竞争场景，是宿主机仿真停顿，不计入结论。
- 忙等线程每秒被抢占次数：`lat_h4` 上 CFS 200 次，EEVDF 234 次；`lat_h4_qos` 上 EEVDF 约 103 次。

| 场景 | CFS | EEVDF |
|---|---|---|
| `fair_n0_n5` 偏差（理论比 3.056） | 1–8‰ | 1–2‰ |
| `fair_n0_n10` 偏差（理论比 9.309） | 1–4‰ | 0–6‰ |
| `pingpong` 一次往返 | 13.7µs | 12.3µs |

公平份额两边都在 1% 以内。`pingpong` 上 EEVDF 快约 10%，但这是 TCG 下的客户机时钟，只能说明 EEVDF 的挑选路径没有变慢，不能换算成实机开销。

## 3. 为什么 EEVDF 的唤醒延迟更差

周期线程每次醒来跑 0.5ms。它在队列上运行的这段时间里，自己的 `vruntime` 走得比平均虚拟时间 V 快，出队时 lag 为负。6.6 版 EEVDF 睡眠时保留 lag（`PLACE_LAG`），只在 `update_entity_lag()` 里夹到 \(\pm\max(2\cdot slice, tick)\)，大约 4ms。CFS 则相反，睡醒的任务按 `GENTLE_FAIR_SLEEPERS` 放到 `min_vruntime` 之前，拿到补偿。

所以在 EEVDF 上，周期线程醒来时经常还不 eligible。`check_preempt_wakeup()` 只在唤醒那一刻调用 `pick_eevdf()`，这时选不中它，不抢占。之后它什么时候变成 eligible 都不会触发重新调度。正在跑的忙等线程要到某次 tick 里 `update_deadline()` 发现自己的 slice 用完，才会让出 CPU：

- 没有 QoS 时忙等线程的 slice 是 2.25ms，要等到下一个 4ms 的 tick。4 个忙等线程时周期线程可能要排两轮，p99 约 8ms。
- 打开 QoS 后忙等线程的 slice 是 14ms，p90 和 p99 都贴着 13.5–15.6ms。周期线程自己的 1ms slice 帮不上忙，因为不 eligible 时它的 deadline 根本不参与比较。

排除过的解释：把 `RUN_TO_PARITY` 关掉单独编了一版（`#14`），`lat_h4` 的 p99 仍是 8.05ms，`lat_h4_qos` 的 p99 是 17.45ms。“当前任务用完 slice 前不许被唤醒抢占”不是主因。

## 4. 对项目结论的影响

- `07` 里“QoS 短 slice 让前台 deadline 更早，所以前台延迟降低”在这个场景下不成立。短 slice 只在任务 eligible 时让它排得更靠前（`09-QEMU对照结果.md` 里选中次数 3.9 倍就是这个效果）。对周期性睡醒的前台线程，起作用的是后台任务的长 slice，它把前台的最坏等待拉长到约 14ms。
- EEVDF 这一版在“公平份额准确”和“挑选开销”上没有退化，可以如实写。
- 不能写“EEVDF+QoS 降低了交互延迟”。

上游后来针对的正是这类问题，这里没有移植：6.12 的 `DELAY_DEQUEUE` 让带负 lag 的任务睡眠时先留在队列上把负 lag 还完，醒来时不再欠账；`PREEMPT_SHORT` 允许更短 slice 的唤醒任务打断 `RUN_TO_PARITY`。在 5.10 上可以考虑的方向：

1. 缩短 `QOS_BACKGROUND` / `QOS_UTILITY` 的 slice。它直接决定前台被后台挡住的最坏时间，改表即可验证。
2. 打开 `HRTICK` 特性，让 slice 按实际长度到期，而不是对齐到 4ms 的 tick。只能削掉取整的部分。
3. 移植 `DELAY_DEQUEUE`。改动大，要重新做第 3 节的不变量检查。

## 5. 局限

- TCG 仿真，时间是客户机时钟，绝对值不代表实机。两颗内核在同一台宿主机上交替启动，比较的是相对差。
- 被测对象是内核线程，不经过用户态和系统调用。
- 原版 CFS 上的 `_qos` 行没有模拟 OHOS 真实的 QoS 策略。实际系统里 `qos_manager` 会按等级改 nice，这对两颗内核是同一套机制，这里固定 nice 0 是为了单独看 slice 的作用。
- 只测了单个隔离 CPU，没有测迁移和多核负载均衡。
