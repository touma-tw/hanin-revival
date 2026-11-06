import ctypes, sys, time
from ctypes import wintypes
u=ctypes.windll.user32
dlg=u.FindWindowW(None,"漢音屬性設定")
def find(text):
    res=[]
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h,l):
        t=ctypes.create_unicode_buffer(128); u.GetWindowTextW(h,t,128)
        if t.value==text: res.append(h)
        return True
    u.EnumChildWindows(dlg,cb,0)
    return res[0] if res else None
for text in sys.argv[1:]:
    h=find(text); print(text.encode('utf-8'), h)
    if h: u.SendMessageW(h,0x00F5,0,0); time.sleep(0.5)
