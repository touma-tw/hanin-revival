import os, subprocess, time, sys
HERE = os.path.dirname(os.path.abspath(__file__))
# start the test, kill the host after the first keys have been typed
p = subprocess.Popen([os.path.join(HERE, 'tiptest64.exe'), 'su{WAIT}{WAIT}3cl3{ENTER}ji3{ENTER}'],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1.8)
t0 = time.time()
subprocess.run(['taskkill', '/F', '/IM', 'hanin_host.exe'], capture_output=True)
print('killed host at +1.8s')
out = p.communicate(timeout=60)[0].decode('utf-8', 'replace')
open(os.path.join(HERE, 'killmid.out'), 'w', encoding='utf-8').write(out)
