"""List raw `syscall` sites in the protection block and decode each obfuscated number. Usage: python -I rawsys.py lib.so"""
import struct, sys, capstone
data = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', data, 0x20); phnum, = struct.unpack_from('<H', data, 0x38)
loads = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56) for i in range(phnum)]
def v2o(v):
    for t, f, off, va, pa, fsz, msz, al in loads:
        if t == 1 and va <= v < va + fsz: return v - va + off
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
lo, hi = 0x6225000, 0x6228200
blob = data[v2o(lo):v2o(lo) + hi - lo]
at = 0
while True:
    at = blob.find(b'\x0f\x05', at)
    if at < 0: break
    va = lo + at; at += 2
    # walk back to a start that decodes cleanly onto the syscall
    for back in range(70, 20, -1):
        ins = list(md.disasm(blob[va - lo - back:va - lo + 2], va - back))
        if ins and ins[-1].mnemonic == 'syscall' and ins[-1].address == va: break
    else: continue
    imm = [i for i in ins if i.mnemonic == 'mov' and '[rbp' in i.op_str and ', 0x' in i.op_str]
    num = None
    if len(imm) >= 2:
        a = int(imm[-2].op_str.split(', ')[1], 16); b = int(imm[-1].op_str.split(', ')[1], 16)
        r = a & 63; rot = ((a << r) | (a >> (64 - r))) & (2**64 - 1) if r else a
        num = (rot ^ b) & 0xffffffff
    regs = [f'{i.mnemonic} {i.op_str}' for i in ins[-8:-1]]
    print(hex(va), 'nr', num, '|', '; '.join(regs))
