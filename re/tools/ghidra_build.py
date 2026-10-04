"""Build (or refresh) the local Ghidra project for VETTE.EXE.

  python re/tools/ghidra_build.py            full rebuild: import + auto-analysis + symbols + export
  python re/tools/ghidra_build.py --refresh  re-apply re/symbols.csv and re-export (no re-import)

Inputs come from Game/ (DOS VETTE.EXE, required; Game/PC98/VETTE.EXE, optional).
Outputs (all gitignored): re/bin/ (unpacked EXE), re/ghidra/Vette.gpr (open in the Ghidra GUI),
re/out/<program>/ (per-function disassembly + decompiled C, functions.tsv).

Ghidra is located via --ghidra, $GHIDRA_INSTALL_DIR, or the newest ~/Tools/ghidra_*.
Close the project in the Ghidra GUI before running this (the project is locked while open).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RE = ROOT / 're'
sys.path.insert(0, str(Path(__file__).parent))
import unexepack  # noqa: E402

DOS_PROGRAM = 'VETTE_unpacked.exe'
PC98_PROGRAM = 'VETTE_PC98.exe'


def find_ghidra(arg):
    candidates = [arg, os.environ.get('GHIDRA_INSTALL_DIR')]
    candidates += sorted(glob.glob(str(Path.home() / 'Tools' / 'ghidra_*')), reverse=True)
    for c in candidates:
        if c and (Path(c) / 'support').is_dir():
            return Path(c)
    sys.exit('Ghidra not found: pass --ghidra or set GHIDRA_INSTALL_DIR')


def ensure_java_home(env):
    if env.get('JAVA_HOME'):
        return
    jdks = sorted(glob.glob(r'C:\Program Files\Java\jdk-2*') + glob.glob('/usr/lib/jvm/*21*'), reverse=True)
    if jdks:
        env['JAVA_HOME'] = jdks[0]


def find_case_insensitive(folder, name):
    if folder.is_dir():
        for p in folder.iterdir():
            if p.name.lower() == name.lower():
                return p
    return None


def prepare_inputs():
    (RE / 'bin').mkdir(exist_ok=True)
    dos_src = find_case_insensitive(ROOT / 'Game', 'VETTE.EXE')
    if not dos_src:
        sys.exit('Game/VETTE.EXE not found (see Game/README.md)')
    dos = RE / 'bin' / DOS_PROGRAM
    if not dos.exists() or dos.stat().st_mtime < dos_src.stat().st_mtime:
        out, _ = unexepack.unpack(dos_src.read_bytes())
        dos.write_bytes(out)
    inputs = [dos]
    pc98_src = find_case_insensitive(ROOT / 'Game' / 'PC98', 'VETTE.EXE')
    if pc98_src:
        pc98 = RE / 'bin' / PC98_PROGRAM
        shutil.copyfile(pc98_src, pc98)
        inputs.append(pc98)
    return inputs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ghidra')
    ap.add_argument('--refresh', action='store_true')
    args = ap.parse_args()

    ghidra = find_ghidra(args.ghidra)
    headless = ghidra / 'support' / ('analyzeHeadless.bat' if os.name == 'nt' else 'analyzeHeadless')
    env = dict(os.environ)
    ensure_java_home(env)
    (RE / 'ghidra').mkdir(exist_ok=True)
    (RE / 'out').mkdir(exist_ok=True)

    common = ['-scriptPath', str(RE / 'ghidra_scripts'),
              '-postScript', 'VetteApplySymbols.java',
              '-postScript', 'VetteExportFunctions.java',
              '-log', str(RE / 'out' / 'ghidra.log'),
              '-scriptlog', str(RE / 'out' / 'ghidra_scripts.log')]
    if args.refresh:
        runs = [['-process', '-noanalysis'] + common]
    else:
        runs = [['-import', str(p), '-overwrite', '-loader', 'MzLoader',
                 '-processor', 'x86:LE:16:Real Mode'] + common for p in prepare_inputs()]
    for extra in runs:
        cmd = [str(headless), str(RE / 'ghidra'), 'Vette'] + extra
        print('>', ' '.join(cmd), flush=True)
        subprocess.run(cmd, env=env, check=True)


if __name__ == '__main__':
    main()
