# Lists translated shaders that contain a function whose body only returns constant 0 (an unresolved global-memory
# helper of the translator): such a shader reads zeros where the console reads memory.
import os, struct, sys, collections
d = sys.argv[1]; MT = os.stat(sys.argv[2]).st_mtime if len(sys.argv) > 2 else 0
hits = collections.Counter(); examples = {}
n = 0
for e in os.scandir(d):
    if not e.name.endswith(".spv") or e.stat().st_mtime < MT: continue
    n += 1
    b = open(e.path, 'rb').read()
    if len(b) < 20: continue
    w = struct.unpack('<%dI' % (len(b)//4), b[:len(b)//4*4])
    consts = {}; i = 5; funcs = []; cur = None
    while i < len(w):
        op = w[i] & 0xffff; wc = w[i] >> 16
        if wc == 0: break
        ins = w[i:i+wc]
        if op == 43 and wc >= 4: consts[ins[2]] = ins[3]                     # OpConstant
        elif op == 54: cur = {'params': 0, 'body': []}                        # OpFunction
        elif op == 55 and cur is not None: cur['params'] += 1
        elif op == 56 and cur is not None: funcs.append(cur); cur = None      # OpFunctionEnd
        elif cur is not None and op != 248: cur['body'].append((op, ins))
        i += wc
    for f in funcs:
        if f['params'] >= 2 and len(f['body']) == 1 and f['body'][0][0] == 254 and consts.get(f['body'][0][1][1], 1) == 0:
            hits[e.name] += 1
print('shaders', n, 'with zero stubs', len(hits))
for k, v in hits.most_common(40): print(v, k)
