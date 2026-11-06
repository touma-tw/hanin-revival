"""Run the non-admin TEST build of the installer in a scenario folder, report the
first dialog it shows (text of all its controls), screenshot it, then kill it."""
import ctypes, subprocess, sys, time
from ctypes import wintypes
from PIL import ImageGrab

ctypes.windll.shcore.SetProcessDpiAwareness(2)
u = ctypes.windll.user32
folder = sys.argv[1]
shot = sys.argv[2]
p = subprocess.Popen([folder + r'\HaninRevival-Setup-TEST.exe'], cwd=folder)
time.sleep(4)

wins = []


@ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def top(h, l):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if pid.value == p.pid and u.IsWindowVisible(h):
        wins.append(h)
    return True


u.EnumWindows(top, 0)
out = []
for h in wins:
    t = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, t, 256)
    out.append('WINDOW: ' + t.value)
    texts = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def child(c, l):
        b = ctypes.create_unicode_buffer(2048)
        u.GetWindowTextW(c, b, 2048)
        if b.value.strip() and u.IsWindowVisible(c):
            texts.append(b.value)
        return True

    u.EnumChildWindows(h, child, 0)
    out += ['  ' + x.replace('\r\n', ' / ') for x in texts]
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(shot)
p.kill()
sys.stdout.buffer.write(('\n'.join(out) + '\n').encode('utf-8'))
