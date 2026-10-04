"""Capture DOSBox Staging reference screenshots of VETTE.EXE for comparison with vette_run (Windows).

Runs the original game in DOSBox Staging 0.83+ (vette-ega.conf: EGA, 1200 cycles, which VETTE's CPU
probe classifies like a 12 MHz 286) from a fresh scratch copy of the game folder, replays a schedule
of key presses and takes raw (unscaled, indexed) screenshots. vette-capture.map moves the screenshot
hotkey to a plain F12, so no modifier key press reaches the game.

Keys go to the DOSBox window through SendInput, so that window must stay in front while this runs:
don't use the keyboard or mouse until it finishes. If the window loses the focus, the run stops
instead of typing into another program. The schedule syncs on the BIOS video mode byte, read
through DOSBox Staging's HTTP API (localhost only). See race.schedule for the format.

    py -3 re/tools/dosbox_reference/capture_reference.py --dosbox <path to dosbox.exe>
        [--game Game] [--schedule race.schedule] [--out re/out/dosbox_reference]

Writes <out>/dosbox/<name>.png per `shot` and <out>/dosbox/capture.log (when each action ran).
<out>/work/ holds the game copy, DOSBox's log and its capture folder, and is recreated every run.
Then: capture_ours.py, and re/tools/compare_frames.py <out>/ours <out>/dosbox --out <out>/diff.
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

import schedule

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]

# --- Win32: find, focus and type into the DOSBox window --------------------------------------------
user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wintypes.WORD), ('wScan', wintypes.WORD), ('dwFlags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG), ('mouseData', wintypes.DWORD),
                ('dwFlags', wintypes.DWORD), ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('ki', KEYBDINPUT), ('mi', MOUSEINPUT)]
    _anonymous_ = ('u',)
    _fields_ = [('type', wintypes.DWORD), ('u', _U)]


INPUT_KEYBOARD, KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 1, 0x1, 0x2, 0x8
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetForegroundWindow.restype = wintypes.HWND
user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]


def window_text(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def find_window(pid, cls='SDL_app'):
    """The visible top-level window of process `pid` with window class `cls` (SDL2: SDL_app)."""
    found = []

    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, buf, 64)
            if buf.value == cls:
                found.append(hwnd)
                return False
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else None


def focus(hwnd):
    """Bring hwnd to the front. Windows only lets the foreground thread do that, so attach to its
    input queue for the call."""
    if user32.GetForegroundWindow() == hwnd:
        return True
    fg = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    me = kernel32.GetCurrentThreadId()
    attached = fg_thread and fg_thread != me and user32.AttachThreadInput(me, fg_thread, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    if attached:
        user32.AttachThreadInput(me, fg_thread, False)
    return user32.GetForegroundWindow() == hwnd


def send_keys(hwnd, codes, down=True, up=True):
    """Send set-1 scan codes (0xE0xx = extended key) as hardware scan codes. Refuses (raises) unless
    hwnd is the foreground window, so keys never land in another program."""
    if not focus(hwnd):
        raise RuntimeError(f'the DOSBox window is not in front (foreground: '
                           f'"{window_text(user32.GetForegroundWindow())}"); stopping')
    events = []
    for code, release in ([(c, False) for c in codes] if down else []) + \
                         ([(c, True) for c in reversed(codes)] if up else []):
        flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if release else 0)
        if code & 0xFF00 == 0xE000:
            flags |= KEYEVENTF_EXTENDEDKEY
        event = INPUT()
        event.type = INPUT_KEYBOARD
        event.ki = KEYBDINPUT(0, code & 0xFF, flags, 0, 0)
        events.append(event)
    arr = (INPUT * len(events))(*events)
    if user32.SendInput(len(events), arr, ctypes.sizeof(INPUT)) != len(events):
        raise RuntimeError(f'SendInput failed ({ctypes.get_last_error()})')


# --- DOSBox Staging HTTP API ----------------------------------------------------------------------
class Api:
    def __init__(self, port):
        self.base = f'http://127.0.0.1:{port}/api/v1'

    def read(self, linear, length):
        with urllib.request.urlopen(f'{self.base}/memory/0x{linear:X}/{length}', timeout=5) as r:
            return r.read()

    def write(self, linear, data):
        req = urllib.request.Request(f'{self.base}/memory/0x{linear:X}', data=data, method='PUT',
                                     headers={'Content-Type': 'application/octet-stream'})
        urllib.request.urlopen(req, timeout=5).close()

    def video_mode(self):
        try:
            return self.read(0x449, 1)[0]
        except OSError:
            return None  # the API isn't up yet


def patch(api, pattern, offset, replacement):
    """Find `pattern` (hex) in conventional memory, which must occur exactly once, and write
    `replacement` (hex) at match + offset. Returns the linear address written."""
    mem = api.read(0, 0xA0000)
    needle = bytes.fromhex(pattern)
    hits, i = [], mem.find(needle)
    while i >= 0:
        hits.append(i)
        i = mem.find(needle, i + 1)
    if len(hits) != 1:
        raise RuntimeError(f'patch pattern {pattern} found {len(hits)} times (need exactly 1)')
    data = bytes.fromhex(replacement)
    at = hits[0] + offset
    api.write(at, data)
    if api.read(at, len(data)) != data:
        raise RuntimeError(f'patch at {at:05X}h did not stick')
    return at


# --- Run -----------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dosbox', default=os.environ.get('DOSBOX_STAGING'),
                    help='dosbox.exe of DOSBox Staging 0.83+ (default: $DOSBOX_STAGING)')
    ap.add_argument('--game', default=REPO / 'Game', help='the original game files (only a copy is mounted)')
    ap.add_argument('--schedule', default=HERE / 'race.schedule')
    ap.add_argument('--out', default=REPO / 're' / 'out' / 'dosbox_reference')
    ap.add_argument('--date', default='10/23/1989', help="DOS date (vette_run's fixed start date)")
    ap.add_argument('--time', default='12:00:00', help="DOS time (vette_run's fixed start time)")
    ap.add_argument('--cycles', type=int, default=1200, help='see vette-ega.conf')
    ap.add_argument('--port', type=int, default=8086, help='HTTP API port')
    a = ap.parse_args()
    if not a.dosbox or not Path(a.dosbox).is_file():
        sys.exit('DOSBox Staging not found: pass --dosbox <dosbox.exe> or set DOSBOX_STAGING')
    game = Path(a.game).resolve()
    if not (game / 'VETTE.EXE').is_file():
        sys.exit(f'VETTE.EXE not found in {game}')
    actions = schedule.parse(a.schedule)

    out = Path(a.out).resolve()
    work, shots = out / 'work', out / 'dosbox'
    game_copy, captures = work / 'game', work / 'capture'
    shutil.rmtree(work, ignore_errors=True)
    for d in (game_copy, captures, shots):
        d.mkdir(parents=True, exist_ok=True)
    for f in game.iterdir():
        if f.is_file() and f.suffix.upper() in ('.EXE', '.BIN'):
            shutil.copy2(f, game_copy)
    for f in shots.glob('*.png'):
        f.unlink()

    cmd = [a.dosbox, '--noprimaryconf', '--nolocalconf', '--conf', str(HERE / 'vette-ega.conf'),
           '--set', f'mapperfile={HERE / "vette-capture.map"}',
           '--set', f'capture_dir={captures}', '--set', f'cpu_cycles={a.cycles}',
           '--set', f'webserver_port={a.port}',
           '-c', f'mount c "{game_copy}"', '-c', 'c:', '-c', f'date {a.date}', '-c', f'time {a.time}',
           '-c', 'vette']
    api = Api(a.port)
    if api.video_mode() is not None:
        sys.exit(f'something already answers on port {a.port} (another DOSBox?); close it or use --port')

    log_lines = []
    t0 = time.perf_counter()

    def log(msg):
        line = f'{time.perf_counter() - t0:8.3f}  {msg}'
        log_lines.append(line)
        print(line, flush=True)

    dosbox_log = open(work / 'dosbox.log', 'w')
    # CREATE_NO_WINDOW: no console window that would take the focus from the emulator window.
    proc = subprocess.Popen(cmd, cwd=work, stdout=dosbox_log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        hwnd = None
        while not hwnd:
            if proc.poll() is not None:
                raise RuntimeError(f'DOSBox exited with code {proc.returncode}')
            if time.perf_counter() - t0 > 15:
                raise RuntimeError('the DOSBox window did not appear')
            time.sleep(0.1)
            hwnd = find_window(proc.pid)
        log(f'DOSBox window up (pid {proc.pid})')

        anchor = 0.0
        for act in actions:
            if proc.poll() is not None:
                raise RuntimeError(f'DOSBox exited before line {act.line}')
            if act.at is not None:
                while time.perf_counter() - t0 < anchor + act.at:
                    time.sleep(0.002)
            if act.verb == 'sync':
                mode, limit = int(act.args[0], 16), time.perf_counter() + float(act.args[1])
                while api.video_mode() != mode:
                    if time.perf_counter() > limit:
                        raise RuntimeError(f'timed out waiting for video mode {act.args[0]}h (line {act.line})')
                    time.sleep(0.005)
                anchor = time.perf_counter() - t0
                log(f'sync: video mode {act.args[0]}h')
            elif act.verb == 'key':
                codes = [int(c, 16) for c in act.args]
                send_keys(hwnd, codes, up=False)
                time.sleep(0.1)
                send_keys(hwnd, codes, down=False)
                log(f'key {" ".join(act.args)}')
            elif act.verb in ('down', 'up'):
                send_keys(hwnd, [int(act.args[0], 16)], down=act.verb == 'down', up=act.verb == 'up')
                log(f'{act.verb} {act.args[0]}')
            elif act.verb == 'shot':
                before = set(captures.glob('*.png'))
                send_keys(hwnd, [0x58])  # F12 (vette-capture.map): screenshot, raw format
                deadline = time.perf_counter() + 5
                while not (new := set(captures.glob('*.png')) - before):
                    if time.perf_counter() > deadline:
                        raise RuntimeError(f'no capture appeared for {act.args[0]}')
                    time.sleep(0.02)
                src = new.pop()
                time.sleep(0.2)  # captures are written on a background thread
                shutil.move(src, shots / f'{act.args[0]}.png')
                log(f'shot {act.args[0]} ({src.name})')
            elif act.verb == 'patch':
                at = patch(api, act.args[0], int(act.args[1]), act.args[2])
                log(f'patch {len(act.args[2]) // 2} bytes at linear {at:05X}h')
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(5)
        dosbox_log.close()
        (shots / 'capture.log').write_text('\n'.join(log_lines) + '\n')
    print(f'captures in {shots}')


if __name__ == '__main__':
    main()
