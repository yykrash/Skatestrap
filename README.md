<p align="center"><img src="launcher/icon.png" width="128" alt="Skatestrap"></p>

# Skatestrap

**Download:** [krashscripts.xyz](https://krashscripts.xyz) or [Releases](../../releases) &middot; **Discord:** [discord.gg/DapFnsQWgB](https://discord.gg/DapFnsQWgB)

Run the Roblox **Android** app on Windows, so games get Roblox's native Luau code generation and run
fast even on old PCs with built-in graphics.

Skatestrap is a small ELF loader (`loader/`) that loads Roblox's x86-64 Android libraries and answers
the Android, Java and Linux calls they make with Windows ones, plus a launcher (`launcher/`) with
login, settings, FastFlags, graphics options and auto-update. It opens the normal Roblox app home
screen, so you can play any game.

## How it works

- `loader/`: ELF loading and linking, a Linux syscall/libc shim, a minimal JNI, EGL/GLES and Vulkan
  on Windows, input, files, networking.
- `launcher/`: WPF app (.NET 9, WPF-UI). Your Roblox login stays on your PC, encrypted with Windows
  DPAPI, and is only ever sent to roblox.com.
- `scripts/`: tools to check and stage a Roblox Android release (`update_apk.sh`) and to regenerate
  the GL/Vulkan/JNI tables.

No Roblox files are in this repository. The launcher downloads the official Roblox Android app on your
own machine and checks it against a pinned hash.

## Building

- Loader: LLVM clang for Windows, plus the Khronos headers in `../dl/khronos/include`.
  Run `sh loader/build.sh`.
- Launcher: .NET 9 SDK. Run `dotnet build launcher/Skatestrap.csproj -c Release`.

## Notes

- This is not an official Roblox client, and Roblox may change or block it at any time.
- It does not touch the Windows Roblox client or its anti-cheat.

## License

Apache License 2.0. See `LICENSE` and `NOTICE`. Thanks to [Mocktail](https://github.com/komaruworld/mocktail).
