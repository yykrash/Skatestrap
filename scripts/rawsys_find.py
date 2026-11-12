"""Find the raw `syscall` sites in libroblox.so and print their offsets as a C initializer (for raw_sys[] in loader.c).

These are the syscalls the engine/protection make directly, not through libc. Two tests, both must pass, so no ordinary
`0f 05` byte pair (inside a constant or mid-instruction) is mistaken for one:
  1. a long run of bytes before the `0f 05` disassembles cleanly and lands exactly on it (back-walk 20..70 bytes);
  2. a syscall number is loaded into rax just before it -- `mov eax/rax, <small imm>` or `movsxd rax, e**` within the
     last 16 instructions. Every real syscall does this; stray byte pairs do not.
Verified == the shipped 16 for APK 2.746 (0 missing, 0 extra). Re-verify the count after an APK bump.

Usage:  python -I rawsys_find.py lib.so          # print the offsets (for write_rawsys.py)
        python -I rawsys_find.py lib.so --nr      # also print the decoded syscall number per site
"""
import re, struct, sys, capstone

WIN = 16  # instructions before the syscall to look for the rax load

def rax_loaded(ins):
    for x in ins[-WIN:-1]:
        s = f'{x.mnemonic} {x.op_str}'
        m = re.match(r'mov (?:eax|rax), (0x[0-9a-f]+)$', s)
        if m and int(m.group(1), 16) <= 0x1ff:
            return True
        if re.match(r'movsxd rax, e(ax|bx|cx|dx|si|di)$', s):
            return True
    return False

data = open(sys.argv[1], 'rb').read()
show_nr = '--nr' in sys.argv
phoff, = struct.unpack_from('<Q', data, 0x20); phnum, = struct.unpack_from('<H', data, 0x38)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

found = []  # (va, nr)
for i in range(phnum):
    t, f, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56)
    if t != 1 or not f & 1:  # PT_LOAD, executable
        continue
    at = off
    while True:
        at = data.find(b'\x0f\x05', at, off + fsz)
        if at < 0:
            break
        v = va + at - off
        scan = at
        at += 2
        # walk back to a start that disassembles cleanly and ends exactly on this syscall
        for back in range(70, 20, -1):
            ins = list(md.disasm(data[scan - back:scan + 2], v - back))
            if ins and ins[-1].mnemonic == 'syscall' and ins[-1].address == v:
                break
        else:
            continue
        if not rax_loaded(ins):  # a real syscall loads its number into rax just before `0f 05`; false pairs do not
            continue
        # decode the syscall number when the obfuscated-immediate pair is present (two `mov [rbp...], 0x..`;
        # rotate the first, xor the second). Only informational (--nr); absence does not drop the site.
        imm = [x for x in ins if x.mnemonic == 'mov' and '[rbp' in x.op_str and ', 0x' in x.op_str]
        nr = None
        if len(imm) >= 2:
            a = int(imm[-2].op_str.split(', ')[1], 16); b = int(imm[-1].op_str.split(', ')[1], 16)
            r = a & 63
            rot = ((a << r) | (a >> (64 - r))) & (2**64 - 1) if r else a
            nr = (rot ^ b) & 0xffffffff
        found.append((v, nr))

found.sort()
if show_nr:
    for v, nr in found:
        print(hex(v), 'nr', nr)
    print(f'// {len(found)} sites', file=sys.stderr)
else:
    print(', '.join(hex(v) for v, _ in found))
    print(f'// {len(found)} sites', file=sys.stderr)
