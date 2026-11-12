// The Android side, rebuilt on Windows: every function libroblox.so imports, in the SysV calling convention.
#pragma once
#include <stdint.h>
#define SYSV __attribute__((sysv_abi))

void shim_init(uint8_t *image, uint64_t span);
void *shim_lookup(const char *name);  // our function or data for an import, or 0
extern uint64_t *skatestrap_tcb;

// What our ANativeWindow* points at: the Win32 window it stands for.
#define SKATE_WINDOW_MAGIC 0x57494e44u  // 'WIND'
typedef struct { uint32_t magic; int32_t refs; void *hwnd; int32_t width, height; } SkateWindow;
