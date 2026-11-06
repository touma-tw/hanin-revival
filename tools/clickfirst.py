import ctypes, sys, time
from ctypes import wintypes
u=ctypes.windll.user32
top=u.FindWindowW(None,"漢音輸入法")
print("top",top)
kids=[]
@ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(h,l):
    c=ctypes.create_unicode_buffer(64); u.GetClassNameW(h,c,64)
    t=ctypes.create_unicode_buffer(128); u.GetWindowTextW(h,t,128)
    r=wintypes.RECT(); u.GetWindowRect(h,ctypes.byref(r))
    kids.append((h,c.value,t.value,u.GetDlgCtrlID(h),(r.left,r.top,r.right,r.bottom),u.IsWindowVisible(h)))
    return True
u.EnumChildWindows(top,cb,0)
for k in kids: sys.stdout.buffer.write((repr(k)+"\n").encode("utf-8"))
if len(sys.argv)>1:
    cid=int(sys.argv[1])
    for k in kids:
        if k[3]==cid and k[5]:
            print("clicking",k[0]); u.PostMessageW(k[0],0x00F5,0,0); break
