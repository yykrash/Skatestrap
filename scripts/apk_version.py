"""Print versionName/versionCode from an APK's binary AndroidManifest.xml. Usage: python -I apk_version.py app.apk"""
import struct
import sys
import zipfile

x = zipfile.ZipFile(sys.argv[1]).read('AndroidManifest.xml')
# string pool chunk right after the 8-byte file header
typ, hsz, size, count, styles, flags, s_start, st_start = struct.unpack_from('<HHIIIIII', x, 8)
utf8 = flags & 0x100
offs = struct.unpack_from('<%dI' % count, x, 8 + hsz)
base = 8 + s_start
strings = []
for o in offs:
    p = base + o
    if utf8:
        n = x[p]; p += 2 if n & 0x80 else 1  # utf-16 length (skipped)
        n = x[p]; p += 1
        if n & 0x80:
            n = ((n & 0x7f) << 8) | x[p]; p += 1
        strings.append(x[p:p + n].decode('utf8', 'replace'))
    else:
        n, = struct.unpack_from('<H', x, p); p += 2
        if n & 0x8000:
            n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', x, p)[0]; p += 2
        strings.append(x[p:p + 2 * n].decode('utf-16le', 'replace'))
# walk start-element chunks for <manifest> attributes
p = 8 + size
while p < len(x):
    ctype, chsz, csize = struct.unpack_from('<HHI', x, p)
    if ctype == 0x102:  # start element
        ns, name, astart, asize, acount = struct.unpack_from('<iiHHH', x, p + 16)
        if strings[name] == 'manifest':
            for i in range(acount):
                ans, aname, araw, vsz, z, vtype, vdata = struct.unpack_from('<iiiHBBI', x, p + 16 + astart + i * asize)
                if strings[aname] in ('versionName', 'versionCode', 'package'):
                    print(strings[aname], strings[araw] if araw >= 0 else vdata)
            break
    p += csize
