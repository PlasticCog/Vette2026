#!/usr/bin/env python3
"""Download the SingleStepTests 80286 real-mode suite and convert it for tests/cpu_singlestep.cpp.

    python tests/tools/fetch_singlestep.py [--dest tests/data/singlestep] [--only 00 F6.6 ...]

Source: https://github.com/SingleStepTests/80286 (v1_real_mode, MOO 1.1 files, MIT licensed).
Hardware-captured on a Harris N80C286-12. The data is gitignored (tests/data/).

Output, under <dest>/80286/:
  raw/<name>.MOO.gz      the downloaded files (kept so conversion can be re-run offline)
  <name>.bin             converted tests, one file per opcode / opcode group (format below)
  index.txt              one line per .bin: name, flags mask, status, test count
  metadata.json, revocation_list.txt

Conversion drops the cycle traces and makes each test self-contained:
  * Final registers are merged with the initial ones (the suite only lists changed registers).
  * RAM is the union of initial and final bytes, each with its initial and expected final value.
    A byte that only appears in the final state starts as 0 (the test harness zeroes RAM).
  * Physical addresses are masked to 20 bits, because our machine wraps at 1 MB (A20 off) while the
    test machine had 16 MB. Tests where two listed addresses collide after masking are flagged.
  * Tests in revocation_list.txt are flagged.

.bin format (little endian):
  header: char magic[4] = "VSS1"; u32 count; u16 flags_mask; u8 status; u8 reserved
  test:   u32 idx
          u8  nbytes; u8 bytes[nbytes]            instruction bytes, including prefixes
          u8  flags                               bit0: address alias after 20-bit mask, bit1: revoked
          u8  exception                           0xFF = none
          u32 flag_address                        20-bit; where the exception pushed FLAGS
          u16 init[14]; u16 final[14]             AX CX DX BX SP BP SI DI ES CS SS DS IP FLAGS
          u16 nram; nram * { u32 addr; u8 init; u8 final }   addr bit31: byte present initially
"""

from __future__ import annotations

import argparse
import concurrent.futures
import gzip
import json
import os
import struct
import sys
import urllib.error
import urllib.request

REPO = "SingleStepTests/80286"
BRANCH = "main"
SUITE_DIR = "v1_real_mode"
RAW_BASE = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/"
TREE_API = f"https://api.github.com/repos/{REPO}/git/trees/{BRANCH}?recursive=1"

# MOO register order (REGS chunk bitmask order).
MOO_REGS = ["ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"]
# Order written to .bin: Reg16 encoding order, then SegReg encoding order, then IP and FLAGS.
OUT_REGS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "es", "cs", "ss", "ds", "ip", "flags"]

STATUS_CODES = {"normal": 0, "alias": 1, "undocumented": 2, "undefined": 3, "fpu": 4, "extension": 5, "prefix": 6}


def http_get(url: str, retries: int = 3) -> bytes:
    last = None
    for _ in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "vette2026-fetch-singlestep"})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:  # noqa: PERF203
            last = e
            if isinstance(e, urllib.error.HTTPError) and e.code == 404:
                raise
    raise RuntimeError(f"download failed: {url}: {last}")


def list_test_files(meta: dict) -> list[str]:
    """Names like '00', 'F6.6'. Uses the GitHub tree API, falling back to probing names from metadata."""
    try:
        tree = json.loads(http_get(TREE_API))
        names = []
        for item in tree.get("tree", []):
            path = item["path"]
            if path.startswith(SUITE_DIR + "/") and path.endswith(".MOO.gz"):
                names.append(path[len(SUITE_DIR) + 1:-len(".MOO.gz")])
        if names:
            return names
    except Exception as e:  # noqa: BLE001 - any failure falls back to probing
        print(f"tree listing failed ({e}); probing names from metadata", file=sys.stderr)
    names = []
    for op, info in meta["opcodes"].items():
        if "reg" in info:
            names += [f"{op}.{r}" for r in info["reg"]]
        else:
            names.append(op)
    return names


def opcode_info(meta: dict, name: str) -> dict:
    if "." in name:
        op, reg = name.split(".")
        return meta["opcodes"].get(op, {}).get("reg", {}).get(reg, {})
    return meta["opcodes"].get(name, {})


# --- MOO parsing --------------------------------------------------------------------------------

def parse_regs(buf: bytes, off: int) -> dict:
    mask = struct.unpack_from("<H", buf, off)[0]
    off += 2
    regs = {}
    for i, name in enumerate(MOO_REGS):
        if mask & (1 << i):
            regs[name] = struct.unpack_from("<H", buf, off)[0]
            off += 2
    return regs


def parse_ram(buf: bytes, off: int) -> list:
    count = struct.unpack_from("<I", buf, off)[0]
    off += 4
    out = []
    for _ in range(count):
        addr, val = struct.unpack_from("<IB", buf, off)
        out.append((addr, val))
        off += 5
    return out


def parse_state(buf: bytes, off: int, length: int) -> dict:
    end = off + length
    state = {"regs": {}, "ram": []}
    while off < end:
        tag = buf[off:off + 4]
        sub = struct.unpack_from("<I", buf, off + 4)[0]
        off += 8
        if tag == b"REGS":
            state["regs"] = parse_regs(buf, off)
        elif tag == b"RAM ":
            state["ram"] = parse_ram(buf, off)
        off += sub
    return state


def parse_moo(data: bytes) -> list[dict]:
    if data[:4] != b"MOO ":
        raise ValueError("not a MOO file")
    hlen = struct.unpack_from("<I", data, 4)[0]
    off = 8 + hlen
    tests = []
    while off < len(data):
        tag = data[off:off + 4]
        length = struct.unpack_from("<I", data, off + 4)[0]
        off += 8
        if tag == b"TEST":
            t = {"idx": struct.unpack_from("<I", data, off)[0]}
            p = off + 4
            end = off + length
            while p < end:
                sub = data[p:p + 4]
                slen = struct.unpack_from("<I", data, p + 4)[0]
                p += 8
                if sub == b"NAME":
                    n = struct.unpack_from("<I", data, p)[0]
                    t["name"] = data[p + 4:p + 4 + n].decode("ascii", "replace")
                elif sub == b"BYTS":
                    n = struct.unpack_from("<I", data, p)[0]
                    t["bytes"] = bytes(data[p + 4:p + 4 + n])
                elif sub == b"INIT":
                    t["initial"] = parse_state(data, p, slen)
                elif sub == b"FINA":
                    t["final"] = parse_state(data, p, slen)
                elif sub == b"HASH":
                    t["hash"] = data[p:p + slen].hex()
                elif sub == b"EXCP":
                    num, faddr = struct.unpack_from("<BI", data, p)
                    t["exception"] = (num, faddr)
                p += slen
            tests.append(t)
        off += length
    return tests


# --- Conversion ---------------------------------------------------------------------------------

def convert(tests: list[dict], flags_mask: int, status: int, revoked: set[str]) -> tuple[bytes, dict]:
    out = bytearray()
    out += b"VSS1" + struct.pack("<IHBB", len(tests), flags_mask, status, 0)
    stats = {"aliased": 0, "revoked": 0}
    for t in tests:
        init_regs = t["initial"]["regs"]
        final_regs = dict(init_regs)
        final_regs.update(t["final"]["regs"])

        ram: dict[int, list] = {}  # 20-bit addr -> [present_initially, init, final]
        seen: dict[int, int] = {}  # 20-bit addr -> original 24-bit addr
        aliased = False
        for addr, val in t["initial"]["ram"]:
            a = addr & 0xFFFFF
            if a in seen and seen[a] != addr:
                aliased = True
            seen[a] = addr
            ram[a] = [True, val, val]
        for addr, val in t["final"]["ram"]:
            a = addr & 0xFFFFF
            if a in seen and seen[a] != addr:
                aliased = True
            seen[a] = addr
            if a in ram:
                ram[a][2] = val
            else:
                ram[a] = [False, 0, val]

        flags = 0
        if aliased:
            flags |= 1
            stats["aliased"] += 1
        if t.get("hash") in revoked:
            flags |= 2
            stats["revoked"] += 1
        exc, faddr = t.get("exception", (0xFF, 0))

        bts = t["bytes"]
        out += struct.pack("<IB", t["idx"], len(bts)) + bts
        out += struct.pack("<BBI", flags, exc, faddr & 0xFFFFF)
        out += struct.pack("<14H", *[init_regs[r] for r in OUT_REGS])
        out += struct.pack("<14H", *[final_regs[r] for r in OUT_REGS])
        out += struct.pack("<H", len(ram))
        for a, (present, iv, fv) in ram.items():
            out += struct.pack("<IBB", a | (0x80000000 if present else 0), iv, fv)
    return bytes(out), stats


def process(name: str, raw_dir: str, out_dir: str, meta: dict, revoked: set[str], force: bool) -> tuple:
    raw_path = os.path.join(raw_dir, name + ".MOO.gz")
    bin_path = os.path.join(out_dir, name + ".bin")
    if not os.path.exists(raw_path):
        try:
            data = http_get(RAW_BASE + f"{SUITE_DIR}/{name}.MOO.gz")
        except urllib.error.HTTPError as e:
            if e.code == 404:  # metadata lists opcodes that have no test file (prefixes, 0F, 64-67, ...)
                return None
            raise
        tmp = raw_path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, raw_path)
    info = opcode_info(meta, name)
    flags_mask = int(info.get("flags-mask", 0xFFFF))
    status = STATUS_CODES.get(info.get("status", "normal"), 0)
    if force or not os.path.exists(bin_path):
        tests = parse_moo(gzip.decompress(open(raw_path, "rb").read()))
        blob, stats = convert(tests, flags_mask, status, revoked)
        tmp = bin_path + ".part"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, bin_path)
        count = len(tests)
    else:
        with open(bin_path, "rb") as f:
            count = struct.unpack_from("<I", f.read(8), 4)[0]
        stats = None
    return name, flags_mask, info.get("status", "normal"), count, stats


def sort_key(name: str):
    op, _, reg = name.partition(".")
    return (int(op, 16), int(reg or "0"))


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=os.path.normpath(os.path.join(here, "..", "data", "singlestep")))
    ap.add_argument("--only", nargs="*", help="convert only these files (e.g. 00 F6.6)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--force", action="store_true", help="re-convert even if the .bin exists")
    args = ap.parse_args()

    out_dir = os.path.join(args.dest, "80286")
    raw_dir = os.path.join(out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)

    meta_path = os.path.join(out_dir, "metadata.json")
    if not os.path.exists(meta_path):
        with open(meta_path, "wb") as f:
            f.write(http_get(RAW_BASE + f"{SUITE_DIR}/metadata.json"))
    meta = json.load(open(meta_path))
    rev_path = os.path.join(out_dir, "revocation_list.txt")
    if not os.path.exists(rev_path):
        with open(rev_path, "wb") as f:
            f.write(http_get(RAW_BASE + "revocation_list.txt"))
    revoked = {ln.strip() for ln in open(rev_path) if ln.strip() and not ln.startswith("#")}

    names = args.only if args.only else list_test_files(meta)
    names = sorted(set(names), key=sort_key)
    print(f"{len(names)} test files -> {out_dir}")

    results = []
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {pool.submit(process, n, raw_dir, out_dir, meta, revoked, args.force): n for n in names}
        for fut in concurrent.futures.as_completed(futs):
            name = futs[fut]
            try:
                r = fut.result()
            except Exception as e:  # noqa: BLE001
                print(f"  {name}: FAILED: {e}", file=sys.stderr)
                failed += 1
                continue
            if r is None:
                continue
            results.append(r)
            extra = ""
            if r[4] and (r[4]["aliased"] or r[4]["revoked"]):
                extra = f" (aliased {r[4]['aliased']}, revoked {r[4]['revoked']})"
            print(f"  {name}: {r[3]} tests{extra}")

    # index.txt lists every converted file present (also ones from earlier runs with --only).
    index = {}
    idx_path = os.path.join(out_dir, "index.txt")
    if os.path.exists(idx_path):
        for ln in open(idx_path):
            parts = ln.split()
            if len(parts) == 4 and os.path.exists(os.path.join(out_dir, parts[0] + ".bin")):
                index[parts[0]] = ln.strip()
    for name, mask, status, count, _ in results:
        index[name] = f"{name} {mask} {status} {count}"
    with open(idx_path, "w") as f:
        for name in sorted(index, key=sort_key):
            f.write(index[name] + "\n")
    total = sum(int(v.split()[3]) for v in index.values())
    print(f"index: {len(index)} files, {total} tests")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
