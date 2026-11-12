"""Print imports that shim_lookup does not yet cover, grouped by library. Reads names compiled into the loader."""
import json, re, sys
from pathlib import Path
imps = json.loads(Path(sys.argv[1]).read_text())
src = "".join(Path(f).read_text() for f in sys.argv[2:])
have = set(re.findall(r'\{\s*"(\w+)"\s*,', src))
for lib, names in (imps.items() if isinstance(imps, dict) else []):
    miss = [n for n in (names if isinstance(names, list) else names.keys()) if n not in have]
    print(f"{lib} {len(miss)}: {' '.join(sorted(miss))}\n")
