"""xref.py LIB 'text': find the string's address and every rip-relative lea that points at it.
Usage: python -I xref.py extract/libroblox.so "Connection timed out after"
"""
import struct, sys
data = open(sys.argv[1], 'rb').read()
needle = sys.argv[2].encode()
# program headers: map file offset -> vaddr
phoff, = struct.unpack_from('<Q', data, 0x20); phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
loads = []
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
    if t == 1: loads.append((off, va, fsz, fl))
def to_va(o):
    for off, va, fsz, fl in loads:
        if off <= o < off + fsz: return o - off + va
def to_off(v):
    for off, va, fsz, fl in loads:
        if va <= v < va + fsz: return v - va + off
targets = {}
start = 0
while True:
    o = data.find(needle, start)
    if o < 0: break
    s = data.rfind(b'\0', 0, o) + 1  # start of the containing string
    targets[to_va(s)] = data[s:data.find(b'\0', o)][:90]
    targets[to_va(o)] = data[o:data.find(b'\0', o)][:90]
    start = o + 1
for v, t in targets.items(): print('string %x %r' % (v, t))
for off, va, fsz, fl in loads:
    if not fl & 1: continue
    seg = data[off:off + fsz]
    for i in range(len(seg) - 7):
        if seg[i] in (0x48, 0x4c) and seg[i + 1] == 0x8d and (seg[i + 2] & 0xc7) == 0x05:
            disp, = struct.unpack_from('<i', seg, i + 3)
            tgt = va + i + 7 + disp
            if tgt in targets: print('lea at %x -> %x' % (va + i, tgt))
