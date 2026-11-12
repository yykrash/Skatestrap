"""Rewrite the raw_sys[] offset list in loader/loader.c (and the "of N" count printed at startup).

Usage:
  python -I scripts/write_rawsys.py loader/loader.c 0x28bdf7b 0x28bdf9e ...   # offsets as args
  python -I scripts/write_rawsys.py loader/loader.c -                          # offsets on stdin, one per line
  python -I scripts/write_rawsys.py --selftest

Only the array initializer and the matching "of <count>" literal are touched; everything else is left alone.
"""
import re
import sys

ARR = re.compile(r'(static const uint64_t raw_sys\[\] = \{)(.*?)(\};)', re.S)
CNT = re.compile(r'(raw syscall sites routed: %d of )(\d+)')


def rewrite(src: str, offsets: list[int]) -> str:
    if not ARR.search(src):
        raise SystemExit('raw_sys[] initializer not found in loader.c')
    body = ' ' + ', '.join('0x%x' % o for o in offsets) + ' '
    out = ARR.sub(lambda m: m.group(1) + body + m.group(3), src, count=1)
    out = CNT.sub(lambda m: m.group(1) + str(len(offsets)), out, count=1)
    return out


def _selftest():
    sample = ('x\nstatic const uint64_t raw_sys[] = { 0x1, 0x2,\n   0x3 };\n'
              'y "raw syscall sites routed: %d of 16\\n" z\n')
    got = rewrite(sample, [0x10, 0x20])
    assert 'raw_sys[] = { 0x10, 0x20 };' in got, got
    assert 'of 2\\n' in got, got
    assert got.count('raw_sys[] = {') == 1
    print('write_rawsys selftest ok')


def main(argv: list[str]) -> None:
    if argv and argv[0] == '--selftest':
        _selftest()
        return
    path = argv[0]
    rest = argv[1:]
    if rest == ['-']:
        rest = [t for t in sys.stdin.read().split() if t]
    offs = [int(t, 16) for t in rest]
    if not offs:
        raise SystemExit('no offsets given')
    src = open(path, encoding='utf-8').read()
    open(path, 'w', encoding='utf-8').write(rewrite(src, offs))
    print('wrote %d raw_sys offsets to %s' % (len(offs), path))


if __name__ == '__main__':
    main(sys.argv[1:])
