#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Preflight or apply the curated snapshot, keeping changed originals in a backup."""
import argparse
import pathlib
import shutil
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('ohos_root', type=pathlib.Path)
p.add_argument('--apply', action='store_true')
a = p.parse_args()
root = a.ohos_root.resolve()
snapshot = pathlib.Path(__file__).resolve().parents[1] / 'repo'
if not (root / 'build.sh').is_file() or not (root / 'kernel/linux/linux-5.10/Makefile').is_file():
    p.error('expected an OpenHarmony source root containing Linux 5.10')
files = sorted(x for x in snapshot.rglob('*') if x.is_file())
# Validate every destination before mutating any file.
for f in files:
    dest = root / f.relative_to(snapshot)
    if not dest.resolve().is_relative_to(root):
        p.error(f'destination escapes source root: {dest}')
changed = [f for f in files if not (root / f.relative_to(snapshot)).is_file()
           or (root / f.relative_to(snapshot)).read_bytes() != f.read_bytes()]
print(f'{len(files)} files checked; {len(changed)} would change')
if a.apply and changed:
    backup = pathlib.Path(tempfile.mkdtemp(prefix='ohos-eevdf-backup-'))
    created = []
    for f in changed:
        rel = f.relative_to(snapshot)
        dest = root / rel
        if dest.exists():
            (backup / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(dest, backup / rel)
        else:
            created.append(str(rel))
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dest)
    (backup / 'NEW_FILES.txt').write_text('\n'.join(created) + '\n')
    print(f'Applied; original files saved at {backup}. NEW_FILES.txt lists added files.')
elif not a.apply:
    print('Dry run only. Review source baseline before using --apply.')
