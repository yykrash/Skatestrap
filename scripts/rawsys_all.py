"""Find every raw `syscall` instruction in libroblox.so (byte scan, kept when most decode starts land on it)."""
import struct, sys, capstone
data = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', data, 0x20); phnum, = struct.unpack_from('<H', data, 0x38)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i in range(phnum):
    t, f, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56)
    if t != 1 or not f & 1: continue
    at = off
    while True:
        at = data.find(b'\x0f\x05', at, off + fsz)
        if at < 0: break
        v = va + at - off; hit = 0
        for back in range(8, 40):
            ins = list(md.disasm(data[at - back:at + 2], v - back))
            hit += bool(ins) and ins[-1].address == v and ins[-1].mnemonic == 'syscall'
        if hit >= 26:
            ins = list(md.disasm(data[at - 30:at + 2], v - 30))
            print(hex(v), hit, '|', '; '.join(f'{x.mnemonic} {x.op_str}' for x in ins[-4:-1]))
        at += 2
