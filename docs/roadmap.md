# Roadmap

## Milestones

| # | Goal | Exit criteria | Rough size |
| --- | --- | --- | --- |
| M0 | Set up the environment | Xcode, Android NDK, and an empty host app running on a device. NDK `.so` files build in `samples/` | 1–2 days |
| M1 | Call a function in a converted `.so` | [m1-elf-to-macho.md](m1-elf-to-macho.md) §7 | A few days to 1 week |
| M2 | libc compatibility shim and on-device signing | `.so` files using `malloc`, `printf`, `pthread`, and TLS work. Conversion through execution completes on the iPhone | A few weeks |
| M3 | Run NativeActivity | The NDK `native-activity` sample renders via ANGLE and responds to touch | 1–2 months |
| M4 | Run a real game | One SDL2 or Godot game with no Java dependency is playable, with audio | Several months |
| M5 | Support ART | The Activity of a simple Java-only app is displayed | Years |

## Items to verify

Items whose design cannot be finalized until checked on a real device. Verify them with a test app in parallel with M1.

| # | Item | Design affected | When |
| --- | --- | --- | --- |
| V1 | Is `TPIDR_EL0` free, and is it preserved across context switches and signals? | How TLS is implemented ([abi-differences.md](../research/abi-differences.md) §2) | M1 |
| V2 | Does XNU preserve or clobber x18? | Whether to reject `.so` files that use x18 | M1 |
| V3 | Can a dylib signed on the iPhone be `dlopen`ed from the container (with a free Apple ID certificate)? | The premise of the whole approach | Start of M2 |
| V4 | Can newly signed dylibs be `dlopen`ed one after another in the same process? | Handling of code added at runtime ([architecture.md](architecture.md) §7) | M2 |
| V5 | Does dyld accept a dylib with no libSystem dependency and empty chained fixups? | How the Mach-O is assembled | M1 |
| V6 | Is the `increased-memory-limit` entitlement available to free accounts? | Whether large games can be handled | M3 |
| V7 | Can dex2oat disable implicit null checks and stack checks? | Whether ART can avoid relying on SIGSEGV | M5 |

## Non-goals

- APKs with only 32-bit ARM (`armeabi-v7a`)
- x86 / x86_64 `.so` files
- Real Google Play Services features (login, billing, cloud saves)
- Games with anti-cheat or DRM
- Distribution via the App Store
