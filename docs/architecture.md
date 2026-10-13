# Design

## 1. Goal and assumptions

Run the native code of Android apps (`lib/arm64-v8a/*.so`) on an iPhone at native speed.

iOS constraints:

- Generating and executing code at runtime (RWX / JIT) is forbidden unless a debugger is attached.
- **Signed code can run.** A dylib signed with the same Team ID as the host app can be `dlopen`ed (proven by LiveContainer).

What Android and iPhone have in common:

- Both are ARM64. The machine code in a `.so` runs as-is on the iPhone's CPU.
- What differs is the file format (ELF / Mach-O), the libc (bionic / libSystem), ABI details, and OS APIs.

So henge **does not translate instructions**. It repacks the ELF as Mach-O, signs it on the iPhone, and lays a compatibility layer for the Android runtime environment (bionic, Android APIs) underneath.

### Comparison with other approaches

![Approach comparison](images/comparison.png)

| Approach | Example | Speed | Resilience to iOS updates | Drawback |
| --- | --- | --- | --- | --- |
| Map ELF directly with JIT | Husk | ◎ | × | Depends on debugger-enabled JIT. Over if Apple closes it |
| Interpreter | LiquidAPK | △ | ◎ | Slow (about 80–90M instructions/s) |
| CPU emulation | Husk (QEMU) | × | ○ | Very slow |
| **AOT conversion + signing (henge)** | — | ◎ | ◎ | Weak against code generated or downloaded at runtime (§7) |

## 2. Overview

![Pipeline](images/pipeline.png)

```
┌──────────────────────── henge host app (installed once via SideStore etc.) ──────────────────────────────┐
│                                                                                                         │
│  ① Import          ② Convert             ③ Sign                 ④ Load                ⑤ Run              │
│  Receive APK  →  ELF → Mach-O dylib  →  Sign with own cert  →  dlopen + mini linker → Start NativeActivity│
│  (share sheet)     (henge-convert)        (on-device signing)    (relocation, init)    (UIKit etc. via shims)│
│                                                                                                         │
│  ┌──────────────────────────────── runtime ────────────────────────────────┐                            │
│  │ libhenge-bionic   libc / libm / libdl / liblog compatibility            │                            │
│  │ libhenge-android  NativeActivity / AAsset / AInput / ANativeWindow / JNI │                            │
│  │ EGL / GLES        ANGLE (Metal backend)                                  │                            │
│  │ Audio             OpenSL ES / AAudio → AVAudioEngine                     │                            │
│  └─────────────────────────────────────────────────────────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Components

| Component | Language | Role |
| --- | --- | --- |
| `henge-convert` | C | Converts ELF `.so` files into Mach-O dylibs. Also patches code (svc, TLS register). Runs both on the iPhone and on macOS |
| Signer | C/Swift | Signs converted dylibs with the same certificate as the host app. An existing implementation (zsign family) is a candidate; license to be checked |
| Mini linker | C | After `dlopen`, reads the ELF dynamic info, resolves relocations, and calls initializers |
| `libhenge-bionic` | C + asm | Exports bionic function names and bridges to Darwin's libSystem. Absorbs ABI differences ([abi-differences.md](../research/abi-differences.md)) |
| `libhenge-android` | C/ObjC | Implements NDK APIs (`libandroid.so`, etc.) with UIKit / Metal / AVFoundation |
| Host app | Swift | APK management UI, runs conversion and signing, hosts the game view |

## 3. Conversion (henge-convert)

Details in [m1-elf-to-macho.md](m1-elf-to-macho.md). Key points:

1. Repack ELF `PT_LOAD` segments into Mach-O `__TEXT` (R / RX) and `__DATA` (RW) **while preserving their relative positions**. Android `.so` files are position-independent and use page-relative references via `adrp`, so as long as relative positions are preserved they work wherever they are loaded.
2. Shift everything by the size of the Mach-O header (16 KiB). A shift that is a multiple of 4 KiB does not change `adrp` results.
3. Code patches (done before signing, so the code can be freely rewritten):
   - Linux `svc #0` → `bl` to the shim's syscall stub
   - `mrs xN, tpidr_el0` → replace with TLS access via the shim if needed (depends on the verification results)
4. Keep the ELF dynamic info (`.dynamic`, `.dynsym`, relocation tables) as-is. dyld does not touch it; the mini linker reads it.
5. Register no initializers in the Mach-O. ELF `DT_INIT` / `DT_INIT_ARRAY` are called by the mini linker after relocation.

**Conditions that cannot be converted (rejected in M1)**

- Code and data share the same 16 KiB page (older `.so` files linked with 4 KiB alignment)
- `DT_TEXTREL` is present (forbidden on Android API 23+, so practically nonexistent)
- APKs that contain only 32-bit ARM

Since November 2025, Google Play has required 16 KiB page support for new apps and updates targeting Android 15 or later. The newer the APK, the more likely it can be converted.

## 4. Signing

- Import the certificate and private key that SideStore / AltStore used at install time (same approach as LiveContainer).
- Place converted dylibs in the app container (`Library/Application Support/henge/<package>/`) and sign them in place.
- The host app and the dylibs share a Team ID, so they pass library validation.
- Free Apple ID certificates expire after 7 days, but only the host app needs to be re-signed. Converted dylibs are re-signed with the same certificate (automatically when the host app launches).
- Status: the built-in signer (`convert/sign.c`) produces ad-hoc signatures (CodeDirectory + empty requirements + empty CMS), which are enough for macOS and the iOS Simulator. Identity signing (the CMS blob with the user's certificate) is still TODO.

## 5. Loading and execution

1. **Resolve NEEDED**: Look at the ELF `DT_NEEDED` entries and load dependencies first.
   - Bundled in the APK (`libc++_shared.so`, `libmain.so`, etc.) → converted dylibs
   - System libraries (`libc.so`, `libm.so`, `libdl.so`, `liblog.so`, `libandroid.so`, `libEGL.so`, `libGLESv2.so`, `libGLESv3.so`, `libOpenSLES.so`, `libaaudio.so`, `libz.so`) → henge shims
2. **dlopen**: `dlopen` the converted dylib. dyld only maps code and data and verifies the signature; it does not interpret the ELF contents.
3. **Find the base address**: Look up our dylib by path in dyld's image list (`_dyld_get_image_header`).
4. **Relocation**: The mini linker rewrites the GOT and pointers in `__DATA`. Symbols are looked up in the order "shims → already loaded dylibs" (matching ELF global lookup order).
5. **Initialization**: Call `DT_INIT`, then `DT_INIT_ARRAY`.
6. **Launch**: Call the entry point such as `ANativeActivity_onCreate` (NativeActivity) or `SDL_main`.

If the app calls `dlopen("libfoo.so")` at runtime, the shim's `dlopen` returns the converted dylib using the same steps.

## 6. Android compatibility layer

### bionic (libc)

- Export functions under bionic names; implement them by calling Darwin's libSystem.
- Convert struct differences (`stat`, `dirent`, `sigaction`, `pthread_mutex_t`, etc.) and constant differences (`errno`, `open` / `mmap` flags, signal numbers).
- **ABI differences are the biggest pitfall.** Variadic arguments, `va_list`, `long double`, etc. are completely different from Darwin. See [abi-differences.md](../research/abi-differences.md).

### NDK API

| Android | iOS implementation |
| --- | --- |
| `ANativeActivity` and lifecycle callbacks | `UIViewController` lifecycle |
| `ANativeWindow` | `UIView` backed by a `CAMetalLayer` |
| EGL / GLES 2 and 3 | ANGLE (Metal backend). Pass the `CAMetalLayer` as `EGLNativeWindowType` |
| Vulkan | MoltenVK (M4 and later) |
| `AInputQueue` / `AInputEvent` | `UITouch`, `GCController` |
| `AAssetManager` | Read `assets/` from the extracted APK |
| OpenSL ES / AAudio | `AVAudioEngine` / AudioUnit |
| `__android_log_print` | `os_log` |
| `AConfiguration`, display info | `UIScreen`, `UITraitCollection` |

### JNI

M1–M4 have no ART. `JavaVM` / `JNIEnv` are given a **fake function table**.

- Common calls (e.g. using `GetStaticMethodID` to get display resolution or locale) return values supplied by henge.
- Unknown calls are logged and return safe values (`NULL`, 0). The log becomes the list of what to implement next.

ART is handled in M5 (§8).

## 7. Code added at runtime

Games that download native code later do not work with AOT conversion alone. However, henge has a signer on the iPhone, so the following may be possible (to be verified):

- When the shim's `dlopen` receives an unconverted ELF, convert, sign, and load it on the spot.
- If a newly signed file can be `dlopen`ed in the same process, no restart is needed.

Self-unpacking packers and DRM that generates code in memory are out of scope.

## 8. Extending to ART (Java / Kotlin) (M5)

- Build ART for `arm64-apple-ios` and embed it in the host app.
- Also build `dex2oat` for iOS and AOT-compile dex **on the iPhone at import time**. The output oat files are ELF, so they are converted to Mach-O and signed just like `.so` files.
- Disable the runtime JIT (`-Xusejit:false`). Methods that are not AOT-compiled run in the interpreter.
- ART uses SIGSEGV for null checks and stack overflow detection. Rather than doing the same on iOS, consider disabling implicit checks in dex2oat (to be verified).
- For the framework (`android.app`, etc.), decide at the start of M5 whether to run the unmodified AOSP `framework.jar` with faked binder and system services (LiquidAPK approach) or reimplement it like ATL.

## 9. Constraints and risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Apple blocks `dlopen` of self-signed dylibs | The whole approach fails | Watch projects using the same approach, such as LiveContainer. Worst case, switch to an interpreter |
| Memory limit (jetsam) | Large games crash | Check whether `increased-memory-limit` is available to free accounts (to be verified) |
| Missed ABI differences | Hard-to-diagnose crashes | List the functions that cross the boundary and write tests ([abi-differences.md](../research/abi-differences.md)) |
| Google Play Services dependency | Login and billing do not work | Out of scope. Use a fake GMS that reports "not signed in" so the app at least starts |
| Licenses | ANGLE (BSD) and MoltenVK (Apache-2.0) are fine. Check when bringing in a signer or ART | Check every time a component is added |
