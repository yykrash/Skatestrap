"""Every `<op> %fs:disp32` in libroblox.so: check its bytes are [64][REX][op][ModRM rm=100 mod=00][SIB 25][disp32]
(9 bytes), and write patches.bin: (u64 address, u32 disp, u8 rex, u8 op, u8 modrm) per site. The loader rewrites
each to [REX][op][ModRM rm=101][rip-rel disp32 to tcb+disp][90 90]: the same instruction on a TCB block, no fs."""
import re, struct, sys, collections
lib = open(sys.argv[1], 'rb').read()
out, shapes, bad = [], collections.Counter(), []
for line in open(sys.argv[2]):
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s+%fs:(0x[0-9a-f]+)', line)
    if not m:
        continue
    at, disp = int(m.group(1), 16), int(m.group(3), 16)
    b = lib[at:at + 9]  # (first LOAD: file offset == vaddr)
    ok = b[0] == 0x64 and 0x48 <= b[1] <= 0x4f and (b[3] & 0xc7) == 0x04 and b[4] == 0x25 and struct.unpack_from('<I', b, 5)[0] == disp
    if not ok:
        bad.append((hex(at), line.strip(), b.hex()))
        continue
    shapes[(m.group(2), disp)] += 1
    out.append(struct.pack('<QIBBB', at, disp, b[1], b[2], b[3]))
print(len(out), 'sites ok', dict(shapes))
print(len(bad), 'odd:', bad[:5])
open(sys.argv[3], 'wb').write(b''.join(out))
