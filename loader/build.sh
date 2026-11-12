#!/bin/sh
# Build loader.exe (run from loader/)
cd "$(dirname "$0")" || exit 1
# FSR 1 shaders -> SPIR-V baked into vk_hand.c; rebuilt only when fsr.comp is newer (glslc from the Vulkan SDK)
for p in easu rcas; do
    if [ fsr/fsr.comp -nt fsr/$p.inc ]; then
        d=; [ $p = easu ] && d=-DEASU
        glslc -fshader-stage=comp $d -O -mfmt=c -o fsr/$p.inc fsr/fsr.comp || exit 1
    fi
done
# window / taskbar icon (the launcher's) baked into loader.exe
MSYS2_ARG_CONV_EXCL='*' "/c/Program Files/LLVM/bin/llvm-rc.exe" -no-preprocess -fo loader.res loader.rc || exit 1
"/c/Program Files/LLVM/bin/clang.exe" -O1 -Wno-deprecated-declarations -Wno-format-zero-length -I../dl/khronos/include loader.c shim.c shim_bulk.c fmt.c gen_gl.c egl_hand.c wgl_egl.c gen_vk.c vk_hand.c threads.c dl.c fs.c jni.c startup.c asset.c net.c tm.c prof.c va_thunks.S setjmp.S loader.res -lsynchronization -lbcrypt -luser32 -lws2_32 -liphlpapi -lpsapi -lopengl32 -lgdi32 -lwinmm -lhid -lsetupapi -lcrypt32 -Wl,/map:loader.map -o loader.exe
