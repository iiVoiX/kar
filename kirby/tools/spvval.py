import os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
d, ref = sys.argv[1], sys.argv[2]
mt = os.stat(ref).st_mtime
files = [e.path for e in os.scandir(d) if e.name.endswith('.spv') and e.stat().st_mtime >= mt]
val = r'C:\VulkanSDK\1.4.363.0\Bin\spirv-val.exe'
def check(f):
    r = subprocess.run([val, '--target-env', 'vulkan1.2', f], capture_output=True, text=True)
    return f, r.returncode, (r.stdout + r.stderr).strip().splitlines()[:1]
bad = []
with ThreadPoolExecutor(16) as ex:
    for f, rc, msg in ex.map(check, files):
        if rc: bad.append((os.path.basename(f), msg))
print('checked', len(files), 'invalid', len(bad))
for b in bad[:10]: print(b)
