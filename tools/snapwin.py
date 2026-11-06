import ctypes, sys
from ctypes import wintypes
from PIL import ImageGrab
u=ctypes.windll.user32
ctypes.windll.shcore.SetProcessDpiAwareness(2)
pid_filter=None
res=[]
@ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(h,l):
    if not u.IsWindowVisible(h): return True
    pid=wintypes.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    import psutil
    try: name=psutil.Process(pid.value).name()
    except Exception: name='?'
    if name.lower()=='hanin_tools.exe':
        t=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,t,256)
        r=wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
        res.append((h,t.value,(r.left,r.top,r.right,r.bottom)))
    return True
u.EnumWindows(cb,0)
for i,(h,t,r) in enumerate(res):
    print(h,t.encode('utf-8'),r)
    if r[2]-r[0]>20:
        ImageGrab.grab(bbox=r, all_screens=True).save(sys.argv[1]+f'_{i}.png')
