"""Disassemble libroblox.so around a virtual address. Usage: python -I dis.py lib.so 0xADDR [before] [after]
Also: python -I dis.py lib.so sym NAME  -> print a dynamic symbol's address."""
import struct
import sys
import capstone

data = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', data, 0x20)
phnum, = struct.unpack_from('<H', data, 0x38)
loads = []
dyn = None
for i in range(phnum):
    t, f, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56)
    if t == 1:
        loads.append((va, off, fsz))
    if t == 2:
        dyn = (va, off)


def v2o(v):
    for va, off, fsz in loads:
        if va <= v < va + fsz:
            return v - va + off
    raise ValueError(hex(v))


if sys.argv[2] == 'sym':
    d = v2o(dyn[0]); tags = {}
    while True:
        tag, val = struct.unpack_from('<qQ', data, d); d += 16
        if tag == 0:
            break
        tags[tag] = val
    st, sy = v2o(tags[5]), v2o(tags[6])
    for i in range(1, (st - sy) // 24):
        nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', data, sy + i * 24)
        name = data[st + nm:data.index(b'\0', st + nm)].decode('latin1')
        if sys.argv[3] in name:
            print(hex(val), shn, name)
    sys.exit()
a = int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x20
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
start = a - before
for ins in md.disasm(data[v2o(start):v2o(start) + before + after], start):
    print('%s%x  %s %s' % ('>' if ins.address == a else ' ', ins.address, ins.mnemonic, ins.op_str))
