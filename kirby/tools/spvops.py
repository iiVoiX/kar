# Offline: opcode histogram, function and block counts of SPIR-V files (what makes a shader big).
import struct, sys, collections, os
NAMES = {54:'Function',248:'Label',65:'AccessChain',61:'Load',62:'Store',247:'SelectionMerge',250:'BranchConditional',
         249:'Branch',251:'Switch',245:'Phi',57:'FunctionCall',124:'Bitcast',128:'IAdd',199:'ShiftRightLogical',
         170:'IEqual',176:'ULessThan',178:'UGreaterThanEqual',166:'LogicalAnd',169:'Select',43:'Constant',
         196:'BitwiseAnd',132:'IMul',5:'Name',71:'Decorate',72:'MemberDecorate',59:'Variable',246:'LoopMerge',
         227:'AtomicIAdd',229:'AtomicExchange',230:'AtomicCompareExchange',80:'CompositeConstruct',81:'CompositeExtract'}
for p in sys.argv[1:]:
    w = open(p, 'rb').read(); n = len(w) // 4; words = struct.unpack('<%dI' % n, w)
    c = collections.Counter(); i = 5
    while i < n:
        op = words[i] & 0xffff; l = words[i] >> 16; c[op] += 1; i += max(l, 1)
    tot = sum(c.values())
    print(os.path.basename(p)[:12], len(w)//1000, 'KB', tot, 'instr;', ', '.join('%s %d' % (NAMES.get(o, o), k) for o, k in c.most_common(12)))
