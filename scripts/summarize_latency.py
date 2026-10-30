#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the latency-profile follow-up evidence and summarize per-boot results."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from statistics import median

LATENCY = ['lat_h0', 'lat_h1', 'lat_h4', 'lat_h1_qos', 'lat_h4_qos']
FAIRNESS = ['fair_n0_n5', 'fair_n0_n10']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'docs/competition/evidence/latency-2026-10-30')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    provenance = json.loads((args.evidence / 'build-provenance.json').read_text())
    require(hashlib.sha256((args.evidence / 'latency-profile.patch').read_bytes()).hexdigest()
            == provenance['patch_sha256'], 'profile patch hash')
    config_hash = hashlib.sha256((args.evidence / 'kernel.config').read_bytes()).hexdigest()
    require(all(provenance[p]['config_sha256'] == config_hash for p in ['baseline', 'hrtick']),
            'build config hash')
    inputs = []

    def read(name, mode, cap, hrtick, inconclusive=False):
        path = args.evidence / name / f'eevdf-{mode}.json'
        data = json.loads(path.read_text())
        content = path.with_suffix('.log').read_bytes()
        require(hashlib.sha256(content).hexdigest() == data['log_sha256'], f'{name}: log hash')
        log_text = content.decode(errors='replace')
        actual_records = [{'line': line, 'fields': dict(re.findall(r'(\w+)=([^\s]+)', line))}
                          for line in log_text.splitlines()
                          if re.search(r'(SCHED_CMP|EEVDF_\w+) ', line)]
        require(data['records'] == actual_records, f'{name}: records differ from log')
        require('set samgr ready ret : succeed' in log_text, f'{name}: samgr log')
        require(data['input_unchanged'] and data['samgr_ready']
                and not data['kernel_fault_lines'], f'{name}: input/boot/fault check')
        # QEMU handles SIGTERM and normally exits 0; some builds exit -SIGTERM.
        require(data['stopped_by_timeout'] and data['returncode'] in [0, -15]
                and 'qemu-system-aarch64: terminating on signal 15' in log_text,
                f'{name}: unexpected QEMU exit')
        require(data['benchmark_complete'] or inconclusive, f'{name}: incomplete benchmark')
        command = data['command']
        boot = command[command.index('-append') + 1].split()
        require(boot.count(f'sched_qos_slice_cap_us={cap}') == 1, f'{name}: cap')
        require(sum(x.startswith('sched_qos_slice_cap_us=') for x in boot) == 1,
                f'{name}: duplicate cap')
        require('-snapshot' in command and '-nic' in command
                and command[command.index('-nic') + 1] == 'none', f'{name}: disk/network isolation')
        expected_image = provenance['hrtick' if hrtick else 'baseline']['image_sha256']
        require(data['input_sha256']['Image-eevdf'] == expected_image, f'{name}: kernel image')
        inputs.append(data['input_sha256'])
        return data

    def compare(name, cap, hrtick):
        data = read(name, 'compare', cap, hrtick)
        cases = {}
        for record in data['records']:
            fields = record['fields']
            if 'case' in fields:
                require(fields['case'] not in cases, f'{name}: duplicate case')
                cases[fields['case']] = fields
        require(set(cases) == set(LATENCY + FAIRNESS + ['pingpong']), f'{name}: case coverage')
        require(all(cases[c]['samples'] == '300' for c in LATENCY), f'{name}: sample count')
        return {
            'run': name,
            'latency_ms': {c: {metric[:-3]: int(cases[c][metric]) / 1e6
                               for metric in ['mean_ns', 'p90_ns', 'p99_ns', 'max_ns']}
                           for c in LATENCY},
            'fairness_error_permille': {c: int(cases[c]['err_permille']) for c in FAIRNESS},
            'pingpong_median_round_us': int(cases['pingpong']['median_round_ns']) / 1000,
        }

    def qos(name, cap, allow_inconclusive=False):
        data = read(name, 'qos', cap, True, allow_inconclusive)
        def one(marker):
            selected = [r['fields'] for r in data['records'] if marker in r['line']]
            require(len(selected) == 1, f'{name}: {marker} coverage')
            return selected[0]
        verdict = one('EEVDF_QOS_BENCH verdict=')['verdict']
        require(verdict == ('inconclusive' if allow_inconclusive else 'pass'), f'{name}: QoS verdict')
        fork = one('EEVDF_FORK_CHECK trials=')
        require(fork['verdict'] == 'pass' and int(fork['trials']) == int(fork['ran']) == 16
                and int(fork['parent_vlag_is_deadline']) == 16
                and int(fork['max_child_lag_ns']) < int(fork['limit_ns']) == 20000000,
                f'{name}: fork')
        rw = one('EEVDF_REWEIGHT_CHECK rounds=')
        require(rw['verdict'] == 'pass' and int(rw['rounds']) == 200
                and int(rw['grouped']) == 1 and int(rw['reweight_queued']) > 0,
                f'{name}: reweight coverage')
        avg = one('EEVDF_AVG_CHECK done ')
        require(avg['verdict'] == 'pass' and int(avg['scans']) > 0
                and int(avg['group_entities']) > 0, f'{name}: AVG coverage')
        for record in [rw, avg]:
            require(all(int(record[k]) == 0 for k in ['bad_avg', 'bad_load', 'bad_min_deadline']),
                    f'{name}: invariant mismatch')
        require(int(rw['over_bound']) == 0, f'{name}: reweight bound')
        on = one('EEVDF_QOS_BENCH phase=slice_on slice=')
        off = one('EEVDF_QOS_BENCH phase=slice_off slice=')
        return {'run': name, 'verdict': verdict,
                'pick_ratio_on': int(on['pick_ratio_x100']) / 100,
                'pick_ratio_off': int(off['pick_ratio_x100']) / 100,
                'cpu_time_ratio_on': int(on['exec_ratio_x100']) / 100,
                'fork_max_child_lag_ms': int(fork['max_child_lag_ns']) / 1e6,
                'reweight_queued': int(rw['reweight_queued']),
                'reweight_max_dv_ns': int(rw['max_dv_ns']),
                'avg_scans': int(avg['scans']), 'avg_queues': int(avg['queues']),
                'avg_group_entities': int(avg['group_entities'])}

    paired = {profile: [compare(f'confirm{n}-{profile}', cap, hrtick) for n in range(1, 4)]
              for profile, cap, hrtick in [('baseline', 0, False), ('latency', 4000, True)]}
    aggregate = {}
    for profile, rows in paired.items():
        aggregate[profile] = {
            'latency_ms': {c: {m: median(r['latency_ms'][c][m] for r in rows)
                               for m in ['mean', 'p90', 'p99', 'max']} for c in LATENCY},
            'fairness_error_permille': {c: [r['fairness_error_permille'][c] for r in rows]
                                        for c in FAIRNESS},
            'pingpong_median_round_us': median(r['pingpong_median_round_us'] for r in rows),
        }
    screening = [compare(name, cap, hrtick) for name, cap, hrtick in [
        ('screen-cap0', 0, False), ('screen-cap4000', 4000, False),
        ('screen-cap2000', 2000, False), ('screen-hrtick-cap4000', 4000, True),
        ('screen-hrtick-cap2000', 2000, True), ('screen-hrtick-cap0', 0, True)]]
    qos_rows = [qos(name, 4000) for name in ['screen-hrtick-cap4000-qos',
                                           'confirm1-latency-qos', 'confirm2-latency-qos']]
    rejected_qos = qos('screen-hrtick-cap2000-qos', 2000, True)
    boot = read('confirm-latency-boot', 'boot', 4000, True)
    require(not boot['records'], 'normal boot ran benchmark hooks')
    for filename in ['ramdisk.img', 'userdata.img', 'system.img', 'vendor.img', 'updater.img']:
        require(len({i[filename] for i in inputs}) == 1, f'shared image differs: {filename}')
    summary = {'date': '2026-10-30', 'comparison_order':
               [f'confirm{n}-{p}' for n in range(1, 4) for p in ['baseline', 'latency']],
               'aggregation': 'median of per-boot statistics, not pooled-sample quantiles',
               'runs_per_profile': 3, 'paired_runs': paired, 'aggregate': aggregate,
               'screening_single_boot': screening, 'qos_runs': qos_rows,
               'rejected_2ms_qos': rejected_qos, 'normal_boot_verified': True,
               'input_hashes_shared': inputs[0],
               'tradeoffs': {'lat_h4_qos_p99_reduction_percent': 100 * (1 - aggregate['latency']
                   ['latency_ms']['lat_h4_qos']['p99'] / aggregate['baseline']['latency_ms']
                   ['lat_h4_qos']['p99']),
                   'pingpong_round_increase_percent': 100 * (aggregate['latency']
                   ['pingpong_median_round_us'] / aggregate['baseline']['pingpong_median_round_us'] - 1)}}
    args.output.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(summary['tradeoffs'], indent=2))


if __name__ == '__main__':
    main()
