"""Imports and exports of an Android ELF: undefined dynamic symbols grouped by version (library), and JNI exports."""
import sys, collections, json
from elftools.elf.elffile import ELFFile
from elftools.elf.gnuversions import GNUVerNeedSection, GNUVerSymSection

f = ELFFile(open(sys.argv[1], 'rb'))
dyn = f.get_section_by_name('.dynsym')
need = {}
vn = next((s for s in f.iter_sections() if isinstance(s, GNUVerNeedSection)), None)
if vn:
    for v, auxs in vn.iter_versions():
        for a in auxs:
            need[a.entry['vna_other']] = (v.name, a.name)
vs = next((s for s in f.iter_sections() if isinstance(s, GNUVerSymSection)), None)
groups, exports = collections.defaultdict(list), []
for i, sym in enumerate(dyn.iter_symbols()):
    if not sym.name:
        continue
    if sym['st_shndx'] == 'SHN_UNDEF':
        ver = vs.get_symbol(i)['ndx'] if vs else 0
        lib = need.get(ver, ('?', '?'))[0] if isinstance(ver, int) else '?'
        groups[lib].append(sym.name)
    else:
        exports.append(sym.name)
total = sum(map(len, groups.values()))
print('imports', total)
for lib, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    print(f'  {lib}: {len(names)}')
print('exports', len(exports), '| JNI:', sorted(n for n in exports if n.startswith('Java_') or n.startswith('JNI_'))[:12], '...', sum(n.startswith('Java_') for n in exports), 'Java_*')
json.dump({k: sorted(v) for k, v in groups.items()}, open(sys.argv[2], 'w'), indent=1)
