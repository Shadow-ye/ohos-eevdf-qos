#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate three alternating CFS/EEVDF runs and aggregate per-boot statistics."""
import json
import hashlib
import pathlib
import statistics

root = pathlib.Path(__file__).resolve().parents[1]
evidence = root / 'docs/competition/evidence'
lat = ['lat_h0', 'lat_h1', 'lat_h4', 'lat_h1_qos', 'lat_h4_qos']
expected = set(lat + ['fair_n0_n5', 'fair_n0_n10', 'pingpong'])
runs = {}
inputs = {}
for kernel in ['cfs', 'eevdf']:
    runs[kernel] = []
    for n in range(1, 4):
        path = evidence / f'2026-10-30-run{n}-{kernel}/{kernel}-compare.json'
        data = json.loads(path.read_text())
        if hashlib.sha256(path.with_suffix('.log').read_bytes()).hexdigest() != data['log_sha256']:
            raise ValueError(f'log hash mismatch {path}')
        if not data['benchmark_complete'] or not data['input_unchanged'] or data['kernel_fault_lines']:
            raise ValueError(f'invalid run {path}')
        inputs[f'{kernel}-{n}'] = data['input_sha256']
        cases = {}
        for record in data['records']:
            f = record['fields']
            if 'case' in f:
                if f['case'] in cases:
                    raise ValueError('duplicate case')
                cases[f['case']] = f
        assert set(cases) == expected, (path, set(cases))
        assert all(cases[x]['samples'] == '300' for x in lat)
        runs[kernel].append(cases)
# Verify shared disks/ramdisk are identical for every kernel and run.
for filename in ['ramdisk.img', 'userdata.img', 'system.img', 'vendor.img', 'updater.img']:
    assert len({x[filename] for x in inputs.values()}) == 1, filename
for kernel in runs:
    assert len({inputs[f'{kernel}-{n}'][f'Image-{kernel}'] for n in range(1,4)}) == 1
summary = {'date': '2026-10-30', 'runs_per_kernel': 3,
           'aggregation': 'median of per-boot statistics, not pooled-sample quantiles', 'latency_ms': {},
           'fairness_error_permille': {}, 'pingpong_median_round_us': {}, 'inputs': inputs}
for kernel, rows in runs.items():
    summary['latency_ms'][kernel] = {case: {metric.removesuffix('_ns'): statistics.median(int(r[case][metric])/1e6 for r in rows)
                                                  for metric in ['mean_ns','p90_ns','p99_ns']}
                                               for case in lat}
    summary['fairness_error_permille'][kernel] = {case: [int(r[case]['err_permille']) for r in rows]
                                                   for case in ['fair_n0_n5','fair_n0_n10']}
    summary['pingpong_median_round_us'][kernel] = statistics.median(int(r['pingpong']['median_round_ns'])/1000 for r in rows)
(evidence/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
lines = ['# 实验结果', '', '## 本次独立复测 2026 年 10 月 30 日', '',
         '使用既有 #12 CFS 与 #15 EEVDF 镜像，顺序为 CFS、EEVDF，交替各三次。原始串口保留于 evidence/2026-10-30-run*；每个 JSON 记录输入与日志哈希。所有比较使用相同用户态镜像。', '',
         '以下延迟单位为毫秒，依次列出平均、p90、p99。每个值是三次启动各自统计量的中位数；每次每场景 300 样本，未合并为 900 样本分位数。', '']
for case in lat:
    a=summary['latency_ms']['cfs'][case]; b=summary['latency_ms']['eevdf'][case]
    lines.append(f"- `{case}`：CFS {a['mean']:.3f} / {a['p90']:.3f} / {a['p99']:.3f}；EEVDF {b['mean']:.3f} / {b['p90']:.3f} / {b['p99']:.3f}。")
lines += ['', '## 公平性和交接', '']
for kernel in runs:
    lines.append(f"- {kernel.upper()}：nice0 对 nice5 的误差（千分比）{summary['fairness_error_permille'][kernel]['fair_n0_n5']}；nice0 对 nice10 {summary['fairness_error_permille'][kernel]['fair_n0_n10']}。pingpong 三次启动中位数 {summary['pingpong_median_round_us'][kernel]:.3f} μs。")
lines += ['', '## QoS 机制与不变量', '',
          '本次 EEVDF QoS 关闭/启用后 pick 次数比分别为 1.00/4.25，CPU 时间比均为 0.98。fork 16/16、子任务最大 lag 2.67 ms；reweight 189 次在树检查，0 次越界，最大漂移 8 ns；AVG done 扫描 2141 次、8600 队列，三项不一致均为 0。samgr 就绪。详见 qos 子目录。', '',
          '独立无测试参数启动也已完成，samgr 就绪，未出现基准测试记录，输入镜像保持不变。证据见 evidence/2026-10-30-boot。', '',
          '## 结论和限制', '',
          '本次数据支持 QoS 改变请求粒度，也再次显示当前 EEVDF 在忙等竞争下唤醒尾延迟退化。14 ms 后台 slice 会延长等待，短前台 slice 不能保证唤醒时 eligible。该原型未达到交互尾延迟优化目标。', '',
          '时间均为 TCG 客户机时钟。被测为绑核内核线程，未覆盖完整用户态 QoS 调用链、SMP 迁移、实机和长期稳定性。pingpong 的变化不能推导硬件调度开销。串口存在最小用户态镜像 watchdog_service 重启信息，未检测到内核 BUG/Oops/panic/RCU stall。', '',
          '## 历史证据', '',
          '9 月 28 日的数据和反证内核记录仍保留于 内核分析/09-QEMU对照结果.md 和 10-CFS与EEVDF对比结果.md。历史数据不冒充本次原始日志。源快照与构建树的逐文件一致性核对不等于 bit-for-bit 重新构建证明。', '']
(root/'docs/competition/实验结果.md').write_text('\n'.join(lines))
print(json.dumps(summary, ensure_ascii=False, indent=2))
