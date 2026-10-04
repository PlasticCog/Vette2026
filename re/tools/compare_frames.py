"""Pixel-exact comparison of emulator frames, e.g. vette_run shots against DOSBox reference captures.

    py -3 re/tools/compare_frames.py OURS REF [--out DIR] [--min-match PCT]

OURS and REF are two image files, or two directories whose images are paired by file name (stem).
Indexed images are compared by their palette RGB, not by index, so a wrong attribute palette or
color decode shows up as a difference while a mere renumbering of the palette does not. (vette_run
BMPs and DOSBox Staging raw PNGs both index by the 16 attribute-controller inputs.)

For each pair this prints the identical-pixel percentage, the bounding box of the differences and
the most common (ours -> ref) color substitutions, and writes <name>_diff.png to --out: ours, the
reference, and the differences (mismatches in magenta over a dimmed copy of the reference), stacked.
Exit status is 1 if any pair is below --min-match (default: report only).
"""
import argparse
import sys
from collections import Counter
from pathlib import Path

from PIL import Image, ImageDraw

EXTS = {'.png', '.bmp', '.gif', '.tga'}
MISMATCH = (255, 0, 255)


def load_rgb(path):
    return Image.open(path).convert('RGB')


def compare(ours_path, ref_path, out_dir):
    a, b = load_rgb(ours_path), load_rgb(ref_path)
    name = Path(ours_path).stem
    if a.size != b.size:
        print(f'{name:16s}  size differs: ours {a.size[0]}x{a.size[1]}, ref {b.size[0]}x{b.size[1]}')
        return name, 0.0
    w, h = a.size
    pa, pb = a.load(), b.load()
    diff = Image.eval(b.convert('L'), lambda v: v // 4).convert('RGB')
    pd = diff.load()
    subs = Counter()
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            ca, cb = pa[x, y], pb[x, y]
            if ca != cb:
                subs[(ca, cb)] += 1
                pd[x, y] = MISMATCH
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    bad = sum(subs.values())
    match = 100.0 * (w * h - bad) / (w * h)
    line = f'{name:16s}  {match:8.4f}% identical  ({bad} of {w * h} pixels differ)'
    if bad:
        line += f'  bbox x {x0}-{x1}, y {y0}-{y1}'
    print(line)
    for (ca, cb), n in subs.most_common(4):
        print(f'{"":18s}{n:7d} px  ours #{ca[0]:02X}{ca[1]:02X}{ca[2]:02X} -> ref #{cb[0]:02X}{cb[1]:02X}{cb[2]:02X}')

    if out_dir:
        label = 12
        sheet = Image.new('RGB', (w, 3 * (h + label)), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for i, (img, text) in enumerate([(a, f'ours: {Path(ours_path).name}'),
                                          (b, f'ref: {Path(ref_path).name}'),
                                          (diff, f'diff: {match:.4f}% identical, {bad} px')]):
            draw.text((2, i * (h + label)), text, fill=(255, 255, 0))
            sheet.paste(img, (0, i * (h + label) + label))
        if w < 480:  # double small frames so the pixels can be inspected
            sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
        sheet.save(Path(out_dir) / f'{name}_diff.png')
    return name, match


def pairs(ours, ref):
    ours, ref = Path(ours), Path(ref)
    if ours.is_file() and ref.is_file():
        return [(ours, ref)]
    if not (ours.is_dir() and ref.is_dir()):
        sys.exit('OURS and REF must both be files or both be directories')
    refs = {p.stem: p for p in ref.iterdir() if p.suffix.lower() in EXTS}
    found = [(p, refs[p.stem]) for p in sorted(ours.iterdir()) if p.suffix.lower() in EXTS and p.stem in refs]
    if not found:
        sys.exit(f'no images with matching names in {ours} and {ref}')
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ours')
    ap.add_argument('ref')
    ap.add_argument('--out', help='directory for the <name>_diff.png images')
    ap.add_argument('--min-match', type=float, help='fail if any pair is below this percentage')
    a = ap.parse_args()
    if a.out:
        Path(a.out).mkdir(parents=True, exist_ok=True)
    results = [compare(o, r, a.out) for o, r in pairs(a.ours, a.ref)]
    if a.min_match is not None and any(m < a.min_match for _, m in results):
        sys.exit(1)


if __name__ == '__main__':
    main()
