"""Replay a DOSBox reference schedule in vette_run and save its frames under the same names.

Every schedule line with `ours=<seconds>` becomes a vette_run event at that emulated time:
`key` -> --key, a `down`/`up` pair -> --hold, `shot` -> --shot (saved as <name>.png). Lines without
`ours=` (sync, patch) only matter for DOSBox.

    py -3 re/tools/dosbox_reference/capture_ours.py [--schedule race.schedule]
        [--vette-run build/rel/vette_run.exe] [--game Game] [--out re/out/dosbox_reference/ours]

Needs Pillow (BMP -> PNG; the PNGs keep vette_run's 16-entry palette).
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

import schedule

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def vette_run_events(path):
    keys, holds, shots, downs = [], [], [], {}
    for a in schedule.parse(path):
        if a.ours is None:
            continue
        if a.verb == 'key':
            keys += [(a.ours, sc) for sc in a.args]
        elif a.verb == 'down':
            downs[a.args[0]] = a.ours
        elif a.verb == 'up':
            holds.append((downs.pop(a.args[0]), a.ours, a.args[0]))
        elif a.verb == 'shot':
            shots.append((a.ours, a.args[0]))
        else:
            sys.exit(f'{path}:{a.line}: ours= is not supported on "{a.verb}"')
    if downs:
        sys.exit(f'{path}: keys pressed but never released: {sorted(downs)}')
    return keys, holds, shots


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--schedule', default=HERE / 'race.schedule')
    ap.add_argument('--vette-run', default=REPO / 'build' / 'rel' / 'vette_run.exe')
    ap.add_argument('--game', default=REPO / 'Game')
    ap.add_argument('--out', default=REPO / 're' / 'out' / 'dosbox_reference' / 'ours')
    a = ap.parse_args()

    keys, holds, shots = vette_run_events(a.schedule)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    end = max([t for t, _ in shots] + [t for _, t, _ in holds] + [t for t, _ in keys]) + 0.5
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [str(a.vette_run), '--game', str(a.game), '--seconds', f'{end:g}', '--out', tmp]
        for t, sc in keys:
            cmd += ['--key', f'{t:g}:{sc}']
        for t0, t1, sc in holds:
            cmd += ['--hold', f'{t0:g}:{t1:g}:{sc}']
        for t, _ in shots:
            cmd += ['--shot', f'{t:g}']
        print(' '.join(cmd[:1] + [c if ' ' not in c else f'"{c}"' for c in cmd[1:]]))
        run = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.exit(f'vette_run failed ({run.returncode}): {run.stderr}')
        for t, name in shots:
            bmp = Path(tmp) / f'shot_{t:06.2f}.bmp'
            if not bmp.exists():
                print(f'  {name}: no frame at {t:g} s (text mode?)')
                continue
            Image.open(bmp).save(out / f'{name}.png')
    print(f'{len(shots)} frames in {out}')


if __name__ == '__main__':
    main()
