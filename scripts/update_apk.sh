#!/bin/sh
# Regenerate the two version-fragile loader inputs from a new Roblox APK, rebuild, selftest. ON-DEMAND, run on this PC.
# Everything else in the loader resolves by JNI symbol name and survives an APK bump; only these two don't:
#   - patches.bin        (the %fs rewrite sites, from scripts/fs_patches.py)
#   - raw_sys[] offsets  (the raw syscall sites in loader/loader.c, from scripts/rawsys_find.py)
# Stops before the game test and the push (both stay manual: game run on a test account; push is outward-facing).
#
# Usage:  bash scripts/update_apk.sh path/to/roblox.apk [--force]
set -e
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
APK=$1
FORCE=$2
OBJDUMP="/c/Program Files/LLVM/bin/llvm-objdump.exe"
[ -n "$APK" ] || { echo "usage: bash scripts/update_apk.sh path/to/roblox.apk [--force]"; exit 1; }
[ -f "$APK" ] || { echo "no such APK: $APK"; exit 1; }

STAGE="$ROOT/build/apkupd"
rm -rf "$STAGE"; mkdir -p "$STAGE"

echo "== APK version =="
VER=$(python -I scripts/apk_version.py "$APK" | awk '/versionName/{print $2}')
echo "versionName: $VER"
CUR=$(cat extract/APK_VERSION.txt 2>/dev/null || echo "")
if [ -n "$VER" ] && [ "$VER" = "$CUR" ] && [ "$FORCE" != "--force" ]; then
    echo "already on $VER (extract/APK_VERSION.txt); pass --force to rebuild anyway."
    exit 0
fi

echo "== extract libroblox.so =="
python -I - "$APK" "$STAGE/libroblox.so" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
cand = [n for n in z.namelist() if n.endswith('/libroblox.so') or n == 'libroblox.so']
cand = [n for n in cand if 'x86_64' in n] or cand
if not cand:
    raise SystemExit('libroblox.so not found in APK')
open(sys.argv[2], 'wb').write(z.read(cand[0]))
print('extracted', cand[0])
PY

echo "== disassemble %fs sites =="
"$OBJDUMP" -d --no-show-raw-insn "$STAGE/libroblox.so" | grep '%fs:' > "$STAGE/fs.lst" || true
echo "$(wc -l < "$STAGE/fs.lst") %fs lines"

echo "== patches.bin =="
ODD=$(python -I scripts/fs_patches.py "$STAGE/libroblox.so" "$STAGE/fs.lst" "$STAGE/patches.bin" | tee /dev/stderr | awk '/odd:/{print $1}')
if [ "${ODD:-0}" != "0" ]; then
    echo "ABORT: $ODD %fs sites did not match the known 9-byte shape. The loader's patch_fs may need changes; investigate before shipping."
    exit 1
fi

echo "== raw_sys offsets =="
python -I scripts/rawsys_find.py "$STAGE/libroblox.so" > "$STAGE/rawsys.txt" 2> "$STAGE/rawsys.count"
cat "$STAGE/rawsys.count"
OFFS=$(grep -oE '0x[0-9a-f]+' "$STAGE/rawsys.txt" || true)
N=$(printf '%s\n' "$OFFS" | grep -c . || true)
[ "$N" -gt 0 ] || { echo "ABORT: rawsys_find found 0 syscall sites (a code-shape change); investigate."; exit 1; }
echo "found $N raw syscall sites (was: 16 for APK 2.746; a big change means the code shape moved -- verify before shipping)"

echo "== commit new inputs (backing up the old) =="
cp loader/loader.c "$STAGE/loader.c.bak"
cp patches.bin "$STAGE/patches.bin.bak" 2>/dev/null || true
printf '%s\n' "$OFFS" | python -I scripts/write_rawsys.py loader/loader.c -
cp "$STAGE/patches.bin" "$ROOT/patches.bin"
cp "$STAGE/libroblox.so" "$ROOT/extract/libroblox.so"
[ -n "$VER" ] && echo "$VER" > extract/APK_VERSION.txt

echo "== build =="
bash loader/build.sh

echo "== selftest =="
ST=$(cd loader && ./loader.exe --selftest 2>&1 | tail -1)
echo "$ST"
case "$ST" in
    *"0 bad"*) ;;
    *) echo "WARNING: selftest did not report '0 bad'. New inputs are in place; restore from $STAGE/*.bak if needed."; exit 1 ;;
esac

cat <<EOF

== done: patches.bin + raw_sys regenerated, loader built, selftest clean ($VER) ==
Next (manual, by the rules):
  1. Game test on a test account:  bash bench/vk.sh apkupd '{}'
  2. If it plays clean, publish: bash scripts/publish_update.sh
Backups of the pre-update loader.c / patches.bin are in $STAGE.
EOF
