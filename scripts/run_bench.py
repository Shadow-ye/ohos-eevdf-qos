#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run existing ARM64 images without modifying them; preserve log and provenance."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import time
from datetime import datetime, timezone


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--images', type=pathlib.Path, required=True)
    p.add_argument('--kernel', choices=['eevdf', 'cfs'], default='eevdf')
    p.add_argument('--mode', choices=['qos', 'compare', 'boot'], default='compare')
    p.add_argument('--seconds', type=int, default=100)
    p.add_argument('--output', type=pathlib.Path, required=True)
    a = p.parse_args()
    if a.mode == 'qos' and a.kernel == 'cfs':
        p.error('QoS invariant hooks require the EEVDF image')
    if a.seconds <= 0:
        p.error('--seconds must be positive')
    a.images = a.images.resolve()
    a.output.mkdir(parents=True, exist_ok=True)
    log = a.output / f'{a.kernel}-{a.mode}.log'
    meta = log.with_suffix('.json')
    if log.exists() or meta.exists():
        p.error('output already exists; choose a fresh directory')
    kernel = a.images / f'Image-{a.kernel}'
    files = [kernel, a.images / 'ramdisk.img']
    args = ['qemu-system-aarch64', '-M', 'virt', '-smp', '4', '-m', '1024',
            '-nographic', '-cpu', 'cortex-a57', '-snapshot', '-nic', 'none', '-monitor', 'none']
    for name, index in [('userdata', 3), ('vendor', 2), ('system', 1), ('updater', 0)]:
        f = a.images / f'{name}.img'
        files.append(f)
        args += ['-drive', f'if=none,file={f},format=raw,id={name},index={index}',
                 '-device', f'virtio-blk-device,drive={name}']
    boot = ('console=ttyAMA0 init=/bin/init hardware=qemu.arm.linux root=/dev/ram0 rw '
            'ohos.required_mount.system=/dev/block/vdb@/usr@ext4@ro,barrier=1@wait,required '
            'ohos.required_mount.vendor=/dev/block/vdc@/vendor@ext4@ro,barrier=1@wait,required')
    boot += {'qos': ' eevdf_qos_bench=1', 'compare': ' sched_cmp_bench=1 isolcpus=3', 'boot': ''}[a.mode]
    args += ['-kernel', str(kernel), '-initrd', str(a.images / 'ramdisk.img'), '-append', boot]
    hashes = {f.name: digest(f) for f in files}
    started = datetime.now(timezone.utc).isoformat()
    start = time.monotonic()
    with log.open('xb') as out:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT)
        timed_out = False
        try:
            proc.wait(timeout=a.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    content = log.read_text(errors='replace')
    records = []
    for line in content.splitlines():
        if re.search(r'(SCHED_CMP|EEVDF_\w+) ', line):
            records.append({'line': line, 'fields': dict(re.findall(r'(\w+)=([^\s]+)', line))})
    complete = {'qos': all(f'{x} verdict=pass' in content for x in ['EEVDF_QOS_BENCH'])
                and all(re.search(x + r'[^\n]*verdict=pass', content) for x in
                        ['EEVDF_FORK_CHECK', 'EEVDF_REWEIGHT_CHECK'])
                and bool(re.search(r'EEVDF_AVG_CHECK done scans=[^\n]*verdict=pass', content)),
                'compare': f'SCHED_CMP kernel={a.kernel} done' in content,
                'boot': 'set samgr ready ret : succeed' in content}[a.mode]
    faults = re.findall(r'^.*(?:BUG:|Oops:|Kernel panic|WARNING:|rcu[^\n]*stall).*$', content, re.M | re.I)
    data = dict(started_utc=started, elapsed_seconds=round(time.monotonic()-start, 2),
                command=args, qemu_version=subprocess.check_output(['qemu-system-aarch64', '--version'], text=True),
                input_sha256=hashes, input_unchanged=all(digest(f) == hashes[f.name] for f in files),
                returncode=proc.returncode, stopped_by_timeout=timed_out,
                benchmark_complete=bool(complete), samgr_ready='set samgr ready ret : succeed' in content,
                kernel_fault_lines=faults, records=records, log_sha256=digest(log))
    meta.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({k: data[k] for k in ['benchmark_complete', 'samgr_ready', 'input_unchanged', 'kernel_fault_lines']}, ensure_ascii=False))
    raise SystemExit(0 if complete and not faults and data['input_unchanged'] else 1)


if __name__ == '__main__':
    main()
