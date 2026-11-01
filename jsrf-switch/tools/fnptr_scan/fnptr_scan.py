"""fnptr_scan -- code pointers in the XBE's data with no lifted function.

    python3 -I tools/fnptr_scan/fnptr_scan.py default.xbe src/recomp/gen/recomp_funcs.h

Lists every dword in .rdata, .data and MMATRIX that points into a code
section (.text, D3D, DSOUND, XGRPH, XPP) where no sub_XXXXXXXX starts:
handler tables and vtables whose targets the function detector merged into a
neighbour, so an indirect call there fails ("[ICALL] Failed to resolve VA").
Most rows are plain numbers that happen to fall in the code range; the real
ones have known entries beside them ("known" = how many of the 8 dwords
around are lifted entries) and start right after a ret or padding. JSRF on
2026-10-25: 0x00038090, 0x00051490, 0x0001FD10, 0x00020F10 (recomp_split.c,
recomp_manual.c). Reads the game's XBE, which is not in the repository.
"""
# Find pointers in the XBE's data that land in code where no lifted function starts.
import re, struct, sys, bisect
xbe_path, funcs_h = sys.argv[1], sys.argv[2]
d = open(xbe_path, 'rb').read()
base = struct.unpack_from('<I', d, 0x104)[0]
nsec, secaddr = struct.unpack_from('<II', d, 0x11C)
secs = []
for i in range(nsec):
    off = secaddr - base + i * 56
    flags, va, vsize, raw, rsize, name_addr = struct.unpack_from('<IIIIII', d, off)
    name = d[name_addr - base:name_addr - base + 16].split(b'\0')[0].decode()
    secs.append((name, flags, va, vsize, raw, rsize))
CODE = ('.text', 'D3D', 'DSOUND', 'XGRPH', 'XPP')
code = [(n, va, va + vs) for n, f, va, vs, r, rs in secs if n in CODE]
data = [(n, va, r, min(rs, len(d) - r)) for n, f, va, vs, r, rs in secs if n in ('.rdata', '.data', 'MMATRIX')]
for s in secs: print('%-10s flags %X va %08X..%08X raw %X+%X' % (s[0], s[1], s[2], s[2]+s[3], s[4], s[5]))
entries = sorted(set(int(m, 16) for m in re.findall(r'\bsub_([0-9A-Fa-f]{8})\s*\(void\)', open(funcs_h).read())))
eset = set(entries)
def in_code(v):
    for n, lo, hi in code:
        if lo <= v < hi: return n
    return None
cands = {}
for n, va, raw, rs in data:
    for o in range(0, rs - 3, 4):
        v = struct.unpack_from('<I', d, raw + o)[0]
        cs = in_code(v)
        if not cs or v in eset: continue
        # neighbours: known entries in the 8 dwords around (a vtable), or
        # all pointing into one function (a switch table)
        nb = [struct.unpack_from('<I', d, raw + o + k * 4)[0] for k in range(-4, 5) if k and 0 <= o + k * 4 < rs - 3]
        known = sum(1 for x in nb if x in eset)
        i = bisect.bisect_right(entries, v) - 1
        host = entries[i] if i >= 0 else 0
        cands.setdefault(v, []).append((n, va + o, known, host))
print(len(entries), 'lifted entries;', len(cands), 'distinct non-entry code pointers in data')
rows = []
for v, refs in cands.items():
    best = max(r[2] for r in refs)
    host = refs[0][3]
    # bytes at the target (file offset of code)
    for n, f, va, vs, r, rs in secs:
        if va <= v < va + rs:
            b = d[r + v - va:r + v - va + 6].hex(); break
    else: b = ''
    rows.append((best, v, host, len(refs), refs[0][0], refs[0][1], b))
rows.sort(key=lambda r: (-r[0], r[1]))
print('target    host      off   refs neigh_known  first_ref           bytes')
for best, v, host, nref, sec, at, b in rows:
    if True:
        print('%08X  %08X  +%-4X %4d %5d        %s@%08X  %s' % (v, host, v - host, nref, best, sec, at, b))
