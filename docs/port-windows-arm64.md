# Porting the host to Windows on Arm (NVIDIA N1X)

**Status: experimental bring-up; no playable Windows build.** This page records verified milestones and how to reproduce them. The [Ocarina of Time game note](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental) records the game-specific boot checkpoint.

The question it answers: this project runs a Switch game's own AArch64 code natively on Apple silicon, and stands in for the Horizon kernel and the SDK's services. Could the same approach work on an NVIDIA N1X-class laptop — Arm cores, a Blackwell GPU, unified LPDDR5X, Windows on Arm — and would Windows' Prism emulation help?

Short answer: **use a native ARM64 host, not Prism.** [M0](#m0--toolchain-and-target-check) verified native Vulkan on the RTX Spark N1X. [M1](#m1--the-rewriters-windows-variant) runs rewritten AArch64 code, [M2](#m2--boot-to-the-games-entry-point) reaches Ocarina's `nnMain`, and [M3](#m3--services-input-audio-and-a-frame-loop-with-no-gpu) services the active game. [M4](#m4--the-vulkan-replay-backend) executes indexed draws from the game and presents its textures in a Win32 window.

## What the current architecture is

Three parts, and only the middle one is the "compatibility layer" in the usual sense:

| Part | Role | Why it matters for a port |
|---|---|---|
| `dm/tools/patch_nso.py` | A static AArch64-to-AArch64 binary rewriter: `svc` → a host gate, `x18`/`tpidr_el0` accesses → host thread slots, `sp` realignment, acquire/release exclusives, spin-wait gates, registered and function-matched cuts | Determined by target-ABI facts, not by Apple. Windows on Arm has the same class of facts ([below](#what-transfers-unchanged)) |
| `dm/host` | The Horizon kernel and the SDK's services: threads and synchronization, memory, filesystem and saves, HID, audio, movies, keyboard, lifecycle. About 48,000 lines | Portable C, but its platform layer is Apple's: 157 files touch `mach_*`, `dispatch_*`, `os_unfair_lock`, CoreAudio, AudioToolbox, VideoToolbox, IOSurface, `CAMetalLayer` |
| `dm/gfx` | NVN reimplemented on Metal by recording commands and replaying them; also Vulkan and desktop GL layers on Metal. About 79,000 lines | The record/replay design and the object-lifetime machinery are API-independent; the replay half is Metal |

Shaders are translated offline, never at run time: `dmsh_*` decodes Maxwell and Ampere shader code into an IR and emits SPIR-V, which SPIRV-Cross turns into Metal Shading Language ([dm/gfx/README.md](../dm/gfx/README.md)).

## Verdict in one table

| | Verdict |
|---|---|
| Rewriter | **Ports.** Same kind of rewrite, new target-ABI table and a gate that calls a Win32 host function |
| Host | **Ports.** Large but mechanical; the semantically delicate parts are the thread scheduler and the memory layout |
| Graphics | **Rebuilt.** A new replay backend (Vulkan preferred); the shader chain gets *shorter*, stopping at SPIR-V |
| Prism | **Not used.** It translates x86/x64 to ARM64; the guest is already AArch64 |
| ARM64EC | A separate ABI for mixing native ARM64EC and emulated x64 code if an x64-only dependency ever requires it. Not needed by the probe or proposed host |

## What transfers unchanged

- **Guest code, images, profiles.** An NSO is AArch64 either way. Per-build knowledge — build IDs, registered cuts, `MATCH_CUTS` fingerprints, per-game constants and shader tables — is platform-independent. Only the memory numbers of the profiles are tuned for a phone and would be re-derived.
- **The rewrite's premise.** Windows-on-Arm reserves `x18` as the thread environment block pointer, exactly as Apple reserves it, so the "the OS claims this register" rewrite category is not new. `tpidr_el0` is likewise OS-owned. Misaligned `sp` frames still fault. Exclusive pairs still need acquire/release ordering on a modern core, which the rewriter already supplies ([the caches and ordering entries](reference/rewriter.md)).
- **The guest memory model.** Keep Horizon's 39-bit guest layout as a separate constraint from the host's address-space limit. M0 reserved a 74 GiB span below `2^39`; the full mapping, protection and aliasing model remains M2 work ([Mac comparison](reference/memory.md#the-macs-alias-region-is-the-switchs-64-gb)).
- **No CPU JIT.** The rewriter produces static code, but the Windows loader still has to map it with executable permissions and satisfy applicable process mitigations. M0 tested reservation, not code execution.

## What has to be rebuilt

**The host's platform layer.** Threads, locks and their priority handoff; the guest block's reservation; the writable alias used by code-writing cheats (`mach_vm_remap` on Apple, a pagefile section mapped twice on Windows, which removes the RWX problem entirely); audio device and movie decoding (WASAPI and FFmpeg or Media Foundation instead of CoreAudio and VideoToolbox); input (SDL gamepads with motion and rumble, plus raw HID for the original Joy-Cons and the Pro Controller's passive sensors); the front end.

**The scheduler's semantics, not its code.** Check guest-core affinity, spin-gate yields and priority handoffs against Win32 scheduling. Using a homogeneous set of host cores is an experiment, not a requirement established on N1X ([thread creation and cores](reference/threads.md#thread-creation-and-cores)).

**The graphics replay backend.** Pool, heap, pipeline, texture and buffer caches, the swapchain, presentation timing. The record format, the object owner and the retire/ghost/pin rules survive as they are. Two changes are structural, not cosmetic:

1. **Target Vulkan SPIR-V.** Skip the MSL build on Windows, but validate SPIR-V and adapt bindings, features and shader ABI to the new backend. D3D12 would require a separate DXIL path; DXC is an HLSL compiler, not a general SPIR-V-to-DXIL converter.
2. **Shared storage does not imply shared addresses.** The Metal backend uses CPU pointers as guest GPU addresses. M0 found that Vulkan can import a small Windows host allocation without an explicit staging upload, but returns a different GPU address. Keep guest addresses and Vulkan device addresses distinct.

## Established facts on the Windows side

- **Address space.** On the tested Windows build, `GetNativeSystemInfo` reports a user VA ceiling near 128 TiB, not 512 GiB. Use explicit allocation bounds for Horizon and preserve guest-facing address limits independently of native Vulkan addresses.
- **Horizon's own region table**, as a cross-check on the numbers the host inferred by measurement. A 39-bit process gets a 64 GB alias region, a 2 GB stack region and an 8 GB heap; a 36-bit process gets a 6 GB alias region, 8 GB heap and no stack region. The first three match what `docs/reference/memory.md` and `docs/reference/threads.md` conclude from game behaviour, including why a provisional profile's sizes looked like a phone's.
- **Prism** translates x86 and x64 to ARM64. It cannot host the guest, because the guest is already ARM64 and runs natively; it cannot make an x64 host work either, since an x64 process cannot execute ARM64 guest code. On the target machine it would only run *other people's* x86 software — for example an x86 Windows Switch emulator — which is a different project with a different performance profile.
- **[ARM64EC](https://learn.microsoft.com/en-us/windows/arm/arm64ec)** is the Windows hybrid ABI for interoperating with x64 code. Plain ARM64 and ARM64EC are not interchangeable ABIs; plain ARM64 is the baseline for this port.

## Prior art and third-party projects

No Windows-native analogue was verified in the initial survey. Existing front ends built on nxnative are listed in [front-end documentation](frontends.md).

| Need | Project | Gives | Caveat |
|---|---|---|---|
| Horizon kernel and SVC semantics | `Atmosphere-NX/Atmosphere` (mesosphere); `Ryujinx` (MIT) | Ground truth, and a second independent implementation to diff this host's answers against. `Ryujinx.HLE` is C#/.NET, so it runs natively on the target machine and doubles as an oracle | Ryujinx is archived; forks carry it on |
| Guest-binary tooling | Existing `dm/tools/nso.py`; [Capstone](https://github.com/capstone-engine/capstone) | Tested on native Windows Python in M0; keep the existing reader | No replacement needed |
| Host platform layer | `pthreads4w`, `swift-corelibs-libdispatch` (both Windows ports), `libsdl-org/SDL` (SDL3: window, gamepads with gyro and rumble), `libusb/hidapi`, `mackron/miniaudio` (WASAPI), FFmpeg or Media Foundation | Compile-then-replace shims for the bulk of the Apple port; finished, tested native components for input, audio and video | Semantics-critical locks and the scheduler must be written in Win32 directly, not shimmed |
| Graphics backend | Vulkan with `VulkanMemoryAllocator`, `volk`/`vk-bootstrap`, `SPIRV-Tools`; DXC for HLSL if a D3D12 path is ever wanted | The backend's plumbing, so the work goes into NVN semantics | Licence-clean, none of them an NVN implementation |
| Replay-layer design to read | `doitsujin/dxvk` (zlib), `HansKristian-Work/vkd3d-proton`, `Zink` (Mesa) for the desktop-GL games | Structure for state tracking, descriptor and heap translation, resource lifetime | Design, not code |
| Toolchain | `microsoft/vcpkg` (`arm64-windows`), `llvm/llvm-project` `aarch64-pc-windows-msvc` | Reproducible builds and `arm64` dependencies | — |

### External references used during bring-up

| Reference | What to consult it for |
|---|---|
| [libnx at `b156b47`](https://github.com/switchbrew/libnx/tree/b156b47) | `nx/include/switch/kernel/svc.h` for Horizon SVC signatures and arbitration modes; `nx/include/switch/nacp.h` for control metadata layout. These document the guest-facing interface, not the Windows implementation. |
| Local yuzu-derived worktree at `9207f4b7cdbf` (`oot-door-upstream`) | `src/core/arm/nce/patcher.cpp` for native gate design; `src/core/hle/kernel/k_address_arbiter.cpp` and `src/core/hle/kernel/svc/svc_address_arbiter.cpp` for wait/wake and update ordering; `src/core/hle/service/acc/acc.cpp` and `errors.h` for account IPC commands and results. Read-only cross-checks, not a build dependency. |
| Microsoft Learn: [Windows ARM64 ABI](https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions), [PE TLS](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-tls-section), [VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2) | Reserved register and stack rules, native TLS, and bounded guest address reservations. See M0–M2 for what the probes verified. |
| Khronos [VK_EXT_external_memory_host](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_external_memory_host.html) | Host-pointer import, alignment, and memory type rules behind the M0 experiment. Actual import and address behavior comes from the N1X probe, not from an assumption about unified memory. |
| [Capstone](https://github.com/capstone-engine/capstone) | AArch64 disassembly of the locally extracted SDK and game images. For NVN, verify the shipped SDK's selected entry against the Apple host instead of assuming that an older emulator's behavior is native. |

Keep external checkouts and game extracts outside the repository. The yuzu-derived trees were consulted as references only; do not copy GPL implementation into the MIT-only port. The older yuzu archive below is separate from the later NCE worktree.

### What a yuzu source tree is worth

A 2023-05-14 mainline Windows build and its source tarball (commit `756203c81`, 2,223 files under `src/`, shallow history, no vendored `externals/`) were inspected. Findings:

- **The single most relevant file is missing.** That commit has no `src/core/arm/nce/` tree; `src/core/arm/` is Dynarmic only. yuzu's native-code-execution path — the one independent implementation of "rewrite the `svc`, run the guest code natively on an ARM64 host" — postdates it. A late-2023 or 2024 tree from the surviving forks is needed for that.
- **The kernel is a C++ port of mesosphere**, file for file: `k_address_space_info.cpp` (the region table above), `board/nintendo/nx/k_memory_layout.cpp`, the per-family `kernel/svc/*.cpp`, `k_page_table*.cpp`, `k_process.cpp`'s free-thread count. This is the most useful part, and it is directly comparable to `hle_svc.c`'s layout and to the `svcGetInfo` types this host answers with 0.
- **It is not ground truth either.** Its default resource limits carry a `TODO` noting the values are placeholders; the host's 96-thread limit, taken from `pm_spec`, is the better figure.
- **Nothing for the graphics shader path**: `shader_recompiler/frontend/` has only `maxwell` and `ir`, so no Ampere decode.
- **Vulkan and Windows-side references that are useful to read**: `video_core/renderer_vulkan/` (64 files: texture and buffer caches, scheduler, descriptor pool, `maxwell_to_vk`, pipeline state, swapchain), `video_core/engines/maxwell_3d.cpp`, `audio_core/sink/{sdl2,cubeb}_sink.cpp`, `input_common/drivers/sdl_driver.cpp`.
- **Licence:** GPLv3 at the root, with mixed per-file notices. Use GPL code as a reference rather than incorporating it into an MIT-only distribution. Verify licence and attribution at file level for any third-party code proposed for reuse.

## Open questions

Each is a measurement, not an argument, and each gates the milestone named beside it.

1. **Can Vulkan support game-sized pools?** M4 verified address-based compute on contiguous pools up to 512 MiB. Guest page lifetimes, sparse mappings and CPU/GPU synchronization at game scale are still open.
2. **Can the Vulkan backend render the game correctly?** The game draws and presents on N1X. Inspect the captured scanout: a successful present alone does not establish which scene or pixels were rendered.
3. **Can the guest block use Horizon's commit, protection and alias operations?** M2 verified heap commit/decommit, RELRO, and `MapPhysicalMemory` on Windows. M3 implements exclusive stack moves (`MapMemory`/`UnmapMemory`); shared memory remains open.
4. **Can Windows preserve guest scheduling semantics?** Measure priority handoffs and spin-gate latency in M3, including an affinity-controlled run.
5. **Which Apple-API call sites are semantically load-bearing** as opposed to mechanical? A pass over the 157 files, sorting them into "shim", "rewrite", and "rethink". Gates M2 and M3.
6. **Does the guest-visible save behavior survive the Windows file model?** The native fixture checks transactional commit and rollback, and the game mounts its save and temporary storage. In-game save/load remains open.

## Plan

Ordered so that each milestone's exit criteria are observable, and so that a wrong answer early is cheap. The bring-up discipline is the one already used for a new game ([bring-up](agents/03-bringup.md)); the shader chain's rules in `AGENTS.md` apply unchanged to any chain tool that is touched.

### M0 — Toolchain and target check

**Result: proceed with native ARM64 and Vulkan.** No game files are required. The [probe](../dm/windows/tools/m0_probe.c) builds with MSVC from an ARM64 developer prompt using the bundled Khronos headers and the System32 Vulkan loader. It checks native execution with `IsWow64Process2` and selects NVIDIA's proprietary Vulkan device rather than the Dozen fallback.

From an **ARM64 Native Tools Command Prompt**, set `REPO` to the checkout and `cd` to a writable scratch directory outside the checkout:

```bat
cl /nologo /std:c11 /O2 /W4 /WX /I "%REPO%\dm\third_party\vulkan-headers\include" "%REPO%\dm\windows\tools\m0_probe.c" /link MinCore.lib
m0_probe.exe
```

On an ASUS ProArt P16 H7607BA (RTX Spark N1X), Windows `10.0.28000.3620`, NVIDIA `621.14`, Vulkan `1.4.357`:

- [VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2) reserved 64 and 74 GiB spans entirely below `2^39`, one at a time, without committing physical memory. The reported maximum application address was `0x7ffffffeffff`, the page size 4 KiB and the allocation granularity 64 KiB.
- No Vulkan memory type combined `DEVICE_LOCAL` and `HOST_VISIBLE`. The proprietary driver offered a device-local type (1) and coherent host-visible types (2 and 3, the latter cached). Nonetheless, [VK_EXT_external_memory_host](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_external_memory_host.html) imported a 16 MiB committed Windows allocation below `2^39` in type 2. The driver reported 4 KiB minimum import alignment. The GPU read bytes written through the original CPU pointer, with no explicit upload copy. CPU `0x10000000` and Vulkan buffer device address `0x0c230000` differed on this run; neither value is a fixed address to use in the port.
- Three runs of 20 samples per path (one discarded warm-up), with byte-for-byte checks on the warm-up and final sample. The measured time includes a CPU fill, queue submission, GPU copy to a common readback buffer and a fence wait. The staged path adds a GPU copy into device-local memory. Range of per-run means: mapped non-local buffer **0.804–1.010 ms**; staged **0.951–1.016 ms**; imported host pointer **0.692–0.755 ms**. These are transfer-path timings, not game performance or shader-read bandwidth. The prototype did not exercise address-based shader access, sparse mappings or GPU writes into the imported region.

**Rewriter toolchain.** Native ARM64 Python 3.13.18 via `uv` built Capstone 5.0.9 from source with MSVC ARM64, CMake and NMake, and loaded the native `lz4` and `zstandard` wheels. Run the smoke tests from the checkout, in the ARM64 developer prompt with `cmake` on `PATH`:

```bat
set PYTHONUTF8=1
set DM_REWRITE_J=1
uv run --no-project --python 3.13 --with capstone==5.0.9 --with lz4==4.4.5 --with zstandard==0.25.0 python -m unittest dm/tools/tests/test_nso.py dm/tools/tests/test_nro_header.py dm/tools/tests/test_exclusive_windows.py
```

At M0 all 26 synthetic tests passed with `DM_REWRITE_J=1`. Without it, two NRO smoke tests failed at POSIX `signal.pthread_sigmask`; the pool also requires `fork`. M1 fixed the direct tool's default (the M0 command still works). No vcpkg bootstrap is needed for this probe. The installed MSYS2 LLVM can target MSVC but lacked MSVC-target compiler-rt for linking; MSVC 19.44 produced the validated executable.

### M1 — The rewriter's Windows variant

**Result: the synthetic Windows ARM64 rewrite and SVC gate run natively on N1X.** The direct rewriter defaults to serial on hosts without POSIX signal masks and `fork`. Its output still targets Apple unless `--target=windows-arm64` is explicitly passed. Windows artifacts use `<module>.winarm64.*`; `fixups.tsv` identifies the target and the extra `KEY_WIN_TLS_IDX` entry.

- The [Windows ARM64 ABI](https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions) reserves `x18` for the TEB and provides only 16 bytes of patching red zone. The Windows stubs look up the thread's [PE TLS](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-tls-section) block through the compiler's `x18 + 0x58` TLS vector, leaving the platform register untouched. The host fills `WIN_TLS` with the PE TLS index times eight, and the other key entries with byte offsets within that TLS block. The fixture round-trips guest `x18`, `TPIDR_EL0` and `TPIDRRO_EL0` without accessing the host's system TLS registers.
- A later local NCE snapshot (`9207f4b7cdbf`, `src/core/arm/nce/patcher.cpp`, `WriteSaveContext` and `WriteSvcTrampoline`) provided a design reference: capture guest registers before crossing into the host. Its TLS register and host stack switch are Linux-specific. No code from that tree was incorporated.
- The [fixture](../dm/windows/tools/m1_guest.S) passes two SVC sites through the [assembly gate](../dm/windows/tools/m1_gate.S) to the [C dispatcher](../dm/windows/tools/m1_probe.c) on two native Windows threads with different TLS seeds. The dispatcher resolves each site through `gates.tsv`, modifies `x0`, and deliberately clobbers volatile state; the fixture checks the return values, selected GPRs, the upper byte of `q0`, NZCV, FPSR and FPCR. The probe stages the image writable, then seals the code RX and the metadata read-only before running it. It reads the patch, key and segment offsets from `fixups.tsv` instead of fixed positions, and checks that writable data starts on a 16 KB page of its own ([Module data on the code's 16 KB page](reference/memory.md#module-data-on-the-codes-16-kb-page)) and that one 16 KB key page sits below the patch area, so the shared rewriter layout is covered on Windows too.
- The M1 probe runs on native thread stacks; do not run game images with it. M2 adds separate guest and native dispatch stacks and supports fixed SP-relative access and proved `br x18` jump tables. Dynamic or overlapping stack accesses, CASP pairs involving `x18`, exclusive windows with `x18`, unwinding across generated blocks and CFG integration remain open.

To repeat the [native test](../dm/tools/tests/test_windows_rewrite.py), use an ARM64 Native Tools Command Prompt with `clang`, `llvm-objcopy` and `cmake` on `PATH`. With `REPO` set as for M0, build in a scratch directory outside the checkout and then run from the checkout:

```bat
set "SRC=%REPO%\dm\windows\tools"
clang --target=aarch64-pc-windows-msvc -c "%SRC%\m1_gate.S" -o m1_gate.obj
clang --target=aarch64-none-elf -c "%SRC%\m1_guest.S" -o m1_guest.o
llvm-objcopy -O binary --only-section=.text m1_guest.o m1_guest.bin
cl /nologo /std:c11 /O2 /W4 /WX "%SRC%\m1_probe.c" m1_gate.obj
set "DM_WINARM64_M1_DIR=%CD%"
set PYTHONUTF8=1
cd /d "%REPO%"
uv run --no-project --python 3.13 --with capstone==5.0.9 --with lz4==4.4.5 python -m unittest -v dm/tools/tests/test_windows_rewrite.py
```

The fixture's seven Apple module artifacts matched the pre-M1 rewriter byte-for-byte. The full game regression harness requires a Mac (`regress.py` imports POSIX `fcntl` and drives the Metal host); no Mac gameplay regressions were run.

### M2 — Boot to the game's entry point

**Result: the entry-point and heap checks passed.** The Windows host reached `nnMain` in [Ocarina of Time v131072](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental) and captured a `DM_SVC=1` trace. A line-for-line comparison with a matching Mac run is open. The original M2 run stopped at an applet-service path; see M3 for the later boot checkpoint.

The [loader](../dm/windows/tools/m2_loader.c) places near-layout Windows images in a bounded, profile-partitioned [VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2) reservation and applies RELA, RELR and symbol relocations before sealing code RX. The [host](../dm/windows/tools/m2_host.c) enters the guest on a guarded stack and switches back to the native stack during SVCs. It follows the initialization order in `dm/host/main.c` and implements the initial memory, thread-query and timer calls; unimplemented SVCs stop with a module offset and arguments. Startup import takeovers mirror the Apple host's existing SDK-service seams, not new game cuts. M3 adds native guest workers. Scheduler handoffs, shared mappings, guest fault recovery and Windows unwinding across guest blocks are open.

The native fixture covers cross-module binding, relocation, guest stack writeback, flags and register preservation across a rewritten jump table, heap shrink/regrowth, `QueryMemory`, alias page recommit, stack moves and copy-back, worker start/exit/wait and TLS isolation, service-manager IPC, RELRO permission lock and clock dispatch. It also checks that Apple-target metadata is rejected before execution.

Build from the same ARM64 developer prompt and scratch directory as M1:

```bat
set "SRC=%REPO%\dm\windows\tools"
call "%SRC%\build_host.cmd" || exit /b 1
set "DM_WINARM64_M2_DIR=%CD%"
set PYTHONUTF8=1
cd /d "%REPO%"
uv run --no-project --python 3.13 --with capstone==5.0.9 --with lz4==4.4.5 python -m unittest -v dm.tools.tests.test_windows_rewrite.WindowsHost
```

For an entry-point run, set `EXEFS` to the matching extracted ExeFS (see the [packer below](#m3--services-input-audio-and-a-frame-loop-with-no-gpu)) and `OUT` to the Windows rewrite directory, both outside the checkout. From the checkout:

```bat
set DM_GAME=0400486024DEC000
for %M in (main subsdk0 sdk) do uv run --no-project --python 3.13 --with capstone==5.0.9 --with lz4==4.4.5 --with zbic python dm/tools/patch_nso.py "%EXEFS%\%M" "%OUT%" %M --target=windows-arm64
set DM_SVC=1
"%DM_WINARM64_M2_DIR%\m2_host.exe" "dm\games\%DM_GAME%\game.tsv" "%OUT%" main subsdk0 sdk
```

Check each rewrite's exit status before using its artifacts. No RomFS, shader build or graphics driver is needed to reproduce this entry-point run.

### M3 — Services, input, audio, and a frame loop with no GPU

**In progress.** For the observed real boot and stop, see the [game checkpoint](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental). The host answers initial applet state, one local launch user (`DM_NICK`, default `Player`), `DMROMFS1` file I/O and VI/NVN bootstrap. SVCs and imports switch to the native stack. Host calls back into the SDK accept only register arguments and cannot unwind across the boundary. Known but unsupported NVN names stop **when called**, not during bulk lookups.

From the checkout in the ARM64 developer `cmd.exe`, set `NUP` to the local game container and `WORK` to an absolute work directory outside the checkout. Use the [repository packer](development.md#the-packing-script-in-detail), then run the M2 rewrite command with this `EXEFS` and an external `OUT`:

```bat
uv run --no-project --python 3.13 python scripts/pack_nup.py "%NUP%" --version 131072 --work-dir "%WORK%\pack"
set "EXEFS=%WORK%\pack\extract\v131072\ExeFS"
set "DM_ROMFS=%WORK%\pack\extract\v131072\RomFS.img"
```

Set `DM_SAVE` to an existing absolute, writable scratch directory. The account save persists there in SQLite on guest `Commit`; the temporary mount is separate and only lasts for the launch. Use a scratch save, not a Mac save folder. `DM_FS=1` logs mounts and file I/O. Leave `DM_AOC_DIR` unset for the update-only NUP: add-on enumeration is empty, and a configured DLC directory is unsupported.

The [native fixtures](../dm/tools/tests/test_windows_rewrite.py) cover the import boundary, workers and synchronization, file and save operations, headless graphics recording, submission and retirement, empty input histories, and logical audio device selection. From the M2 scratch directory, build the M3 fixtures before running `WindowsHost` as in M2:

```bat
call "%SRC%\build_host.cmd" || exit /b 1
set "DM_WINARM64_M2_DIR=%CD%"
set "DM_WINARM64_M3_DIR=%CD%"
```

Set `DM_WINARM64_GAME_REWRITE` to the absolute `OUT` before running `WindowsHost` to include `test_sdk_software_opus_pcm`. It uses the rewritten game SDK to decode a known packet and checks that the PCM contains signal. The game's [SDK CPU renderer](reference/audio.md#running-the-sdks-own-renderer-on-the-cpu) runs at 48 kHz, but Windows uses a timed silent sink, not speaker output.

For the AV1 title movie, place native ARM64 `libdav1d-7.dll` beside `m2_host.exe`. `build_host.cmd` copies the DLL from `DM_DAV1D_DLL` (an absolute file path) or from `%SystemDrive%\msys64\clangarm64\bin` when present. It is loaded and ABI-checked on first decoder initialization, not at process startup. `WindowsHost.test_sdk_av1_nv12` checks decoded guest-visible pixels and timestamps through the rewritten SDK.

**Frame loop verified on N1X.** The real game sustains draws, compute dispatches and window presents during its title movie. A scripted-input comparison against a Mac gameplay checkpoint is still open.

#### Resume the Windows bring-up loop

Use this loop for the next native boot blocker. Start from the dated/logged [game checkpoint](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental) and reproduce it rather than assuming an old unsupported call still stops the game. Keep the Apple host and the default rewrite target unchanged.

1. **Reproduce the stop.** Reuse the unchanged NUP, packed RomFS, and matching `--target=windows-arm64` rewrite of `main`, `subsdk0`, and `sdk`. Rebuild the native host after an import-table change (the assembly thunks and C lookup both use [`m3_imports.inc`](../dm/windows/tools/m3_imports.inc)); a host-only fix does not require rewriting the game. Start with `DM_FS=1` and `DM_SVC` unset. Save stdout, stderr, and exit status under the external work folder, with a fresh log for each run. Use a bounded run (start at 60–90 seconds here), and enable full SVC logging only to answer a specific sequencing question: it changes startup timing. Repeat the same boot before calling a new outcome stable. Distinguish a named `[RED]` call, a guest fault with module-relative PC/LR, and a timeout. Keep the intermittent allocator fault separate from a reproducible NVN stop unless a trace connects them.
2. **Verify the contract.** For NVN, check the signature in [`nvn_protos.h`](../dm/gfx/nvn/nvn_protos.h) or `include/nvn.h`, the Apple behavior in [`hle_nvn.c`](../dm/gfx/nvn/hle_nvn.c), and the local SDK implementation when the header or Apple behavior is incomplete. Follow the SDK's proc lookup from the name to its function in the extracted module; use offsets only as evidence for that build, not as host hooks. If the answer depends on what the guest actually passes, temporarily log the relevant arguments or caller at the import, without dumping game data, then remove the probe. For a fault rather than a missing function, disassemble the faulting path and identify the bad value and its writer before making a fix. Use the [external references](#external-references-used-during-bring-up) for ABI and Horizon cross-checks. Finish this step knowing what observable result, memory update, or ordering the guest expects.
3. **Test at the guest import.** Add or extend a native `m3_*_guest.S` fixture in [`dm/windows/tools`](../dm/windows/tools/) and wire it into [`test_windows_rewrite.py`](../dm/tools/tests/test_windows_rewrite.py). Use the same rewritten import or proc-address lookup as the game. When practical, run it against the old host first and confirm it hits the same missing call. Check actual return values, retained state, or backing bytes, plus a relevant failure case; a synthetic present alone is not coverage. The graphics fixture fills its single NRO text page: use a focused fixture for larger changes rather than overlapping code and data.
4. **Implement only the verified behavior.** Keep the Windows import and host changes general; leave unsupported cases diagnostic. A production debug-label entry that disassembles to `ret` can be ignored; a real upload cannot. For command work, keep recording and replay separate: snapshot CPU-side descriptors and strides at recording time, resolve backing and read source bytes at submission, and test the effect at the right point. Do not count a lookup, recording, upload, or fixture present as a game frame. Work through additional verified calls if the requested scope allows it; ask before changing the milestone, backend design, or game policy.
5. **Rebuild, validate, and trace forward.** Build native ARM64 with MSVC `/W4 /WX`, run `/analyze`, then use the [strict local native check](#integration-test-update-upstream-without-losing-the-windows-baseline) with the M1, host and rewrite fixture paths. An ordinary `unittest` success can include native skips. Boot the same NUP again and report how many runs passed the old stop, what the first new stop was, and any faults or timeouts. The general `scripts/regress.py check --games` runner requires a Mac (Windows hits its `fcntl` import); report that limit rather than treating native checks as Mac gameplay coverage.
6. **Leave a handoff.** Put build-specific arguments, call sites and repeat counts in the game note; put the general contract in one subsystem reference entry and link to it. Regenerate `docs/agents/symptoms.md` with `python dm/tools/docs_symptoms.py` after changing a reference entry. Update the next step in this document. Report what is proven (fixture, real boot, or both), the first verified blocker, and which effects still do not run.

For step 1, use the same `cmd.exe` environment from above, from the checkout. Set `BOOT_LOG` to a fresh path under `WORK` and prepare the [offline shader library](#m4--the-vulkan-replay-backend) before the windowed run:

```bat
set "DM_GAME=0400486024DEC000"
set "DM_SVC="
set "DM_FS=1"
set "DM_AOC_DIR="
set "DM_WINDOW=1"
set "DM_SAVE=%WORK%\windows-save"
if not exist "%DM_SAVE%\." mkdir "%DM_SAVE%"
set "DM_VK_SHADERS=%WORK%\vk-shaders-title"
"%DM_WINARM64_M2_DIR%\m2_host.exe" "dm\games\%DM_GAME%\game.tsv" "%OUT%" main subsdk0 sdk > "%BOOT_LOG%" 2>&1
echo Exit: %ERRORLEVEL%
```

Unset `DM_WINDOW` and `DM_VK_SHADERS` to run headless. `DM_WINDOW=1` alone exercises Win32 swapchain presentation; add `DM_VK_SHADERS` for translated draw replay. Window creation and successful Vulkan presents are logged separately. Neither a visible empty window nor a fixture present is a game frame.

The windowed host exposes one FullKey controller on the first supported player ID. Connect an XInput pad before launching or while the game is running. Buttons follow their positions (Xbox **B** is Switch **A**).

With no XInput pad, optionally run `set "DM_KEYBOARD=1"` in the same `cmd.exe` before the windowed launch. Focus the game window, then use **Z** or **Enter** for A, **X** or **Backspace** for B, **C/V** for X/Y, **WASD** for the left stick, **IJKL** for the right, and the arrow keys for the D-pad. **F/G** click the sticks; **Q/E** are L/R; **1/3** are ZL/ZR; **Tab/Esc** are Plus/Minus. Home/Guide is a system button, not part of the game's FullKey state. This option does not capture desktop shortcuts or report a keyboard controller while the game window is unfocused. Unset it with `set "DM_KEYBOARD="`.

For deliberate input without focusing the game window, set `DM_CTL` to a fresh, existing control file before launching. For example, in PowerShell: `$env:DM_CTL = 'C:\work\controls.txt'; [IO.File]::WriteAllText($env:DM_CTL, '')`. From any shell with access to the same file, append `Add-Content -LiteralPath 'C:\work\controls.txt' -Value 'A~0.25' -Encoding ascii`. Each complete line is one press: use the Switch button names, `+` for chords, `LUp` / `RRight` and similar for stick directions, and `~seconds` for a hold (default 0.15 s). Lines starting with `#` are ignored. Keep appending; do not replace or truncate the file during a run. This opt-in FullKey source takes precedence over XInput and the keyboard, even headless. The host logs the press and release returned by the guest's Npad reads. Apple `DM_CTL` diagnostic commands and `DM_KEYS` timed replay files are not supported on Windows.

#### Run the Windows forest checkpoint

Use [`scripts/windows_checkpoint.py`](../scripts/windows_checkpoint.py) for the **copied-save** route. Start each diagnostic series by fetching the upstream branch into a new external pin directory. This is a fetch, not a pull. The default is `origin/main`; choose `--remote` and `--branch` if the authoritative branch is elsewhere.

```powershell
$series = Join-Path $env:WORK ('series-' + (Get-Date -Format yyyyMMdd-HHmmss))
python scripts/windows_checkpoint.py start-series --series-dir "$series"
if ($LASTEXITCODE) { throw 'Upstream pin failed; no series started' }
```

Reuse `$series` for all comparable runs, including runs from a separate integration worktree. `run` does not fetch. Point `$forestSave` at the existing forest SQLite database, not the title-only save produced by a fresh boot. `$run` must be a new directory outside the checkout. Run one player at a time.

```powershell
$run = Join-Path $env:WORK ('forest-' + (Get-Date -Format yyyyMMdd-HHmmss))
python scripts/windows_checkpoint.py run --host "$env:DM_WINARM64_M2_DIR\m2_host.exe" `
  --rewrite "$env:OUT" --romfs "$env:DM_ROMFS" --shaders "$env:DM_VK_SHADERS" `
  --save "$forestSave" --series-dir "$series" --run-dir "$run" --overlay-capture
```

The runner backs up the database, creates `input.txt` **before** starting the player and sends the file-select inputs. It checks for the successful open of `save:/slot_03/progress.sav` and the first mesh submission; neither establishes gameplay. From another terminal run `python scripts/windows_checkpoint.py status --run-dir "$run"` using the **same absolute run path**. Check the last accepted present and its age, foreground state and HID acknowledgment before sending more input. Leave at least 5 GiB of physical memory and commit available; the runner stops its player if either reserve falls below that limit.

`build_host.cmd` produces the optimized player, its matching `m2_host.pdb` and `m2_cpu_trace.exe` together. The runner watches an in-memory count of **accepted Vulkan presents**, not the age of the sampled log or screenshot. After the first present, a 30-second gap triggers one short process-wide CPU sample. Use `--stall-seconds N` to change the cutoff, or `0` to disable profiling (including when using a preserved player without symbols). The guard and timeout remain active while sampling.

Read `status.json` → `cpu_trace` for the trigger time, accepted-present count, sampled frame number and trace outcome. `stall-cpu.txt` and the frozen `stall-frame.ppm` are beside the other captures. The image is the most recent **available capture**, not necessarily the last accepted present; compare the recorded numbers. A thread sampled in a wait is not evidence that it used CPU. A partial trace or missing symbols is not a clean profiling result.

`--overlay-capture` is for diagnosis, not timing comparisons. It arms after the checkpoint and observes the selected pane without changing the game's draw. Compare the attachment just before and after the draw, then compare opacity on consecutive accepted presents. A nonblack image before the pane and a black image after it localize the loss to the pane; an already black input does not. Unavailable evidence does not mean zero opacity. Omit the option for baseline timing; keep the same inputs and observation window on both hosts.

The capture stops its player after publishing the black-present manifest and the next accepted present's result, even if the selected shader was absent. `status` reports `overlay.result` with both opacity readings and the manifest name; image paths are relative to the run directory. A timeout (`--timeout`, seconds from launch preparation; default 1800) or memory-guard stop exits nonzero and leaves partial evidence. Keep `host.log`, `status.json`, `present.tsv` and the capture files together. A completed capture is not a gameplay pass.

To check the runner without game assets, run `python -m unittest dm.tools.tests.test_windows_checkpoint`. Set `DM_WINARM64_M2_DIR` to a current `build_host.cmd` output to include the native crop fixture: an identical black upload must match a blit covering the presented crop, but not a neighboring crop outside its raster bounds.

#### Integration test: update upstream without losing the Windows baseline

The question is whether **upstream integration alone** changes the Kokiri Forest result. Do not combine it with a new optimization. The [Windows game checkpoint](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental) names the baseline executable, shader library, copied save, logs and timings.

**Fresh terminal on the N1X: start here.** `WORK`, `OUT`, and `DM_*` are **not** machine settings. Earlier `set`/`$env:` commands only affected that shell and its children. To **resume the recorded series**, run this in PowerShell from the **integrated** checkout (`nxnative-win-integration`, branch `test/windows-arm64-f856c78d`); do not fetch again or use the title-screen shader library. The older `nxnative` checkout stays untouched as rollback evidence: it holds the `ea5ab250` baseline and its run. For another machine, substitute its external work directory, run and series.

This clone is a **fork** of `seedds/nxnative`. `origin` is the upstream project, for fetching series pins only; never push to it. Add the fork once and push branches there:

```powershell
git remote add fork https://github.com/<account>/nxnative.git   # once
git push -u fork test/windows-arm64-f856c78d
```

```powershell
$ErrorActionPreference = 'Stop'
$env:REPO = (Resolve-Path (git rev-parse --show-toplevel)).Path
Set-Location $env:REPO
$scratch = Join-Path $HOME 'arm64work'
$python = Join-Path $scratch 'venv\Scripts\python.exe'
$env:WORK = Join-Path $scratch '0400486024DEC000-work\nxnative-m2'
$run = Join-Path $env:WORK 'forest-profile-1795485704635991700'  # preserved baseline; never pass as a new --run-dir
$series = Join-Path $env:WORK 'diag-series-1795485366073555900'
$baseline = Get-Content -Raw -LiteralPath "$run\manifest.json" | ConvertFrom-Json
$pin = Get-Content -Raw -LiteralPath "$series\series.json" | ConvertFrom-Json
if ($pin.upstream.commit -ne $baseline.series.upstream.commit) { throw 'Wrong series for this baseline' }
$r = $baseline.runtime
$env:OUT = $r.rewrite.path
$env:DM_ROMFS = $r.romfs.path
$env:DM_VK_SHADERS = $r.shader_library.path
$forestSave = $r.original_save.path
$env:DM_DAV1D_DLL = Join-Path (Split-Path $r.host.path) 'libdav1d-7.dll'
foreach ($p in @($python, "$run\tracked.patch", $r.host.path, $r.pdb.path, "$env:OUT\sdk.winarm64.patched.bin",
                  $env:DM_ROMFS, "$env:DM_VK_SHADERS\graphics.vkc", $forestSave, $env:DM_DAV1D_DLL)) {
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing baseline input: $p" }
}
& $python scripts/windows_checkpoint.py status --run-dir $run
```

This preflight reads artifacts; it does not launch the game. Continue at **step 2** with `$run` and `$series` still in scope. The `DM_WINARM64_M1_DIR` / `M2_DIR` / `M3_DIR` variables are **not** inputs you need to recover: step 3 builds fresh candidate fixtures and the strict gate sets those variables for its tests.

1. **Freeze the comparison.** Start a series as above. Each run's `manifest.json` identifies the upstream pin, local source state, tracked patch, inventory of untracked files, player, symbols, shader library and source/copied save. Check the local divergence **before** experimenting. The older baseline used local `ea5ab250` **plus uncommitted sources** versus then-upstream `208b699c`; do not substitute that historical SHA for the new series pin. Keep the existing checkout, executable, rewrite, RomFS, shader library, save and logs intact. Do not pull, stash or clean the baseline.

   Finish builds and tests before starting the run: the manifest hashes the working tree's nonignored untracked files and the external runtime inputs before launch. A locked or disappearing file is a setup failure, not an excuse to omit it from the inventory. `tracked.patch` includes local tracked commits and edits relative to the merge base; untracked entries are names and hashes, **not backups**. Preserve their contents separately. The rewrite is identified by directory and the ROMFS by path/size/mtime, not by a full image digest.
2. **Integrate only at a checkpoint.** Use the recorded baseline patch and pin in a separate worktree. In PowerShell, `$run` names the baseline run and `$series` its pin. Stop on any command error.

   ```powershell
   $up = (Get-Content -Raw "$series/series.json" | ConvertFrom-Json).upstream.commit
   $patch = (Resolve-Path "$run/tracked.patch").Path
   git worktree add -b "test/windows-arm64-$($up.Substring(0,8))" ../nxnative-win-integration $up
   git -C ../nxnative-win-integration apply --3way --index $patch
   ```

   Review the untracked list and copy the port sources, build scripts and tests to the **same relative paths** in the candidate. The patch does **not** contain them. If upstream already has a path, compare it rather than overwriting it. Do not copy `.git`, `.venv`, caches, game files or keys. Compare the resulting file list with the baseline inventory. Resolve conflicts in the candidate only; check shared rewriter, NVN and host behavior on both platforms. Regenerate `docs/agents/symptoms.md` with `python dm/tools/docs_symptoms.py` after merging reference entries. Require empty `git ls-files -u` output and clean `git diff --check` and `git diff --cached --check` results before building.
3. **Build and gate the candidate.** After resolving conflicts and copying the untracked sources, use the same PowerShell session. On this N1X, Visual Studio supplies ARM64 `cl` and MSYS2 supplies `clang`/`llvm-objcopy`. Do not build into the preserved player's directory.

   ```powershell
   $candidate = (Resolve-Path '..\nxnative-win-integration').Path
   $src = Join-Path $candidate 'dm\windows\tools'
   $build = Join-Path $env:WORK ('integration-build-' + (Get-Date -Format yyyyMMdd-HHmmss))
   $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
   if (!$vs) { throw 'Visual Studio ARM64 C++ tools not found' }
   Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=arm64 -host_arch=arm64'
   if ($env:VSCMD_ARG_TGT_ARCH -ne 'arm64') { throw 'Expected an ARM64 target compiler' }
   $env:Path = "C:\msys64\clangarm64\bin;$env:Path"
   Get-Command cl, clang, llvm-objcopy -ErrorAction Stop | Out-Null
   New-Item -ItemType Directory -Path $build | Out-Null
   Push-Location $build
   try {
       clang --target=aarch64-pc-windows-msvc -c "$src\m1_gate.S" -o m1_gate.obj
       if ($LASTEXITCODE) { throw 'M1 gate assembly failed' }
       clang --target=aarch64-none-elf -c "$src\m1_guest.S" -o m1_guest.o
       if ($LASTEXITCODE) { throw 'M1 guest assembly failed' }
       llvm-objcopy -O binary --only-section=.text m1_guest.o m1_guest.bin
       if ($LASTEXITCODE) { throw 'M1 guest extraction failed' }
       cl /nologo /std:c11 /O2 /W4 /WX "$src\m1_probe.c" m1_gate.obj
       if ($LASTEXITCODE) { throw 'M1 build failed' }
       & "$src\build_host.cmd"
       if ($LASTEXITCODE) { throw 'Candidate host build failed' }
   } finally { Pop-Location }
   Set-Location $candidate
   & $python scripts/check-windows-native.py --build-dir $build --rewrite $env:OUT
   if ($LASTEXITCODE) { throw 'Native gate failed' }
   ```

   The command enables real WSI, points the tests at the chosen build and rewritten SDK, and fails for missing fixtures or skips. A macOS CI pass does not substitute for this check. Run the [Vulkan replay fixtures](#repeat-the-vulkan-replay-fixtures) separately against the preserved shader library; check `/analyze` for new diagnostics. If a changed rewriter or shader ABI requires regenerated artifacts, keep those in separate directories and label the run accordingly; it is no longer a host-only comparison.
4. **Run the same forest load.** Use the candidate executable and game profile, not the old launcher. Use identical RomFS, rewrite, shader library, `DM_OPMODE=0`, input sequence and capture settings. Give each run its own SQLite backup of the same save and load `slot_03/progress.sav`; never point the candidate at the original save. Create the `DM_CTL` file before starting. Run only one player at a time, keep it focused, and retain the baseline's 5 GiB free physical-memory and commit guard. Use a common observation window after the loading icon disappears (for example 15 minutes; the baseline had a single 452-second interval). Record a timeout or guard stop explicitly.

   For the recorded N1X run, reuse its capture thresholds and **leave `$run` pointing at the baseline**:

   ```powershell
   $trial = Join-Path $env:WORK ('integration-run-' + (Get-Date -Format yyyyMMdd-HHmmss))
   & $python scripts/windows_checkpoint.py run --host "$build\m2_host.exe" --rewrite $env:OUT `
     --romfs $env:DM_ROMFS --shaders $env:DM_VK_SHADERS --save $forestSave --series-dir $series `
     --run-dir $trial --overlay-capture --stall-seconds $baseline.capture_settings.stall_seconds `
     --timeout $baseline.capture_settings.timeout_seconds
   ```
5. **Compare equivalent frames, not just frame numbers.** Save the host log, timestamped presents, captures and input acknowledgments. Verify the slot read and first mesh draw. Starting at disappearance of the loading icon, compare consecutive frame intervals, longest interval without a present, first visible forest frame, and bytes imported versus staged on the same pass. Record the first `[RED]`, crash or timeout. Repeat an apparent improvement on another fresh save copy. Passing tests or reaching a black present is not a gameplay pass: capture visible stick-driven movement and report its frame rate. If the checkpoint works, separately start an empty save and verify the opening-to-control transition.
6. **Decide before changing anything else.** If the candidate regresses, keep the baseline launcher and isolate the responsible change in the test worktree. If both runs stay black, capture the scene before and after the final UI overlay **in the same frame** and compare opacity on the next frame. Only after recording the integration result, test individual Windows adaptations of upstream pool-admission changes (`3dd1f7a6`, `9143b6ff`), one per comparison. Record the result in the game note. Before promotion, run the [Mac regression check](agents/06-dev-loop.md#4-checking-a-change-on-every-game-scriptsregresspy) for affected shared code, or mark it untested. Leave the baseline available for rollback.

**Result (2026-11-23):** `f856c78d` is integrated in `test/windows-arm64-f856c78d` at commit `47cb5ea5` (`windows/host: integrate the ARM64 port onto upstream f856c78d`), from the recorded patch and untracked sources. The [strict native gate](../scripts/check-windows-native.py) passes, and the forest load matched the series baseline on the checkpoint lines, overlay result, imported bytes and log content, with no `[RED]` ([game note](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental)). Three things needed adapting to upstream: the relayout's in-text scan, which corrupted data words that decode like a reference ([Module data on the code's 16 KB page](reference/memory.md#module-data-on-the-codes-16-kb-page)); the M1 probe's fixed layout offsets, which the data-page relayout moved; and upstream's newer `test_nso_match` case, which execs a `patch_nso.py` slice that names the port's `OUTPUT_NAME`. Open: the Mac regression check for the shared rewriter change, and a Windows rewrite regenerated under the new layout.

### M4 — The Vulkan replay backend

**New Game reaches the opening on N1X.** The real Ocarina swapchain showed the title menu, accepted New Game and presented the opening movie while the guest loaded Kokiri Forest. See the [game checkpoint](../dm/games/0400486024DEC000/README.md#windows-arm64-bring-up-experimental) for the capture, inputs and build. Movie playback alone does not establish controllable gameplay.

For a black screen after the movie, distinguish an authored fade from a missing stage: keep the last **presented** image, log the first failed draw with both shader keys, and test the rebuilt pair on the replay device. The shorter copied forest-save route can expose a missing shader; it does not by itself prove a fresh file got past the introduction.

The native [address probe](../dm/windows/tools/m0_probe.c) and its [compute shader](../dm/windows/tools/m4_address_probe.comp) tested both imported and staged contiguous 16 MiB and 512 MiB pools on N1X. The shader read one translated device address and wrote another. CPU verification checked the output and guards after the barrier and fence; the imported path read back through the original host pointer without a copy. This does not test a guest page being decommitted or remapped while the GPU still uses it.

```bat
glslangValidator -V --target-env vulkan1.2 "%REPO%\dm\windows\tools\m4_address_probe.comp" -o m4_address_probe.spv
spirv-val --target-env vulkan1.2 m4_address_probe.spv
rem Build m0_probe.exe as under M0, then:
m0_probe.exe --address-shader m4_address_probe.spv 16
m0_probe.exe --address-shader m4_address_probe.spv 512
```

Draws and compute dispatches are synchronous and staged; the pool address probe above does not establish zero-copy replay. The backend supports indexed vertex/fragment draws and reflected compute pipelines using buffers, sampled and storage images, and texel buffers. Sparse mappings and arbitrary guest shader libraries remain outside this checkpoint.

NVN2 `ReportCounter(TIMESTAMP_TOP)` and `ReportCounter(TIMESTAMP)` write a 16-byte `{0, timestamp}` record in submission order, not during recording. The raw timestamp is in [NVN2 nanoseconds](reference/frame-rate.md#nvn2-timestamps-are-nanoseconds) on the guest's process-relative Windows [QPC clock](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps), shared by `nvnDeviceGetCurrentTimestampInNanoseconds` and the guest CPU system tick (which has its own raw unit). Both reports sample the synchronous replay boundary. Their differences include CPU replay, transfer and fence-wait time; they do not measure GPU pipeline time. Other counter types are not implemented, rather than reported as zero.

#### Build the offline shader library

From the checkout, open PowerShell from the ARM64 developer prompt after setting `WORK` and `DM_ROMFS` as above. Use a .NET 10 SDK and Khronos `spirv-val.exe` and `spirv-opt.exe` in the same directory on `PATH`. [The builder](../dm/windows/tools/win_nvn_shaders.py) extracts the selected NVN2 shaders directly from the packed image. This v131072 startup set combines the Layout2 archive's keys with the three other keys from the first 15 render records in the Mac manifest:

```powershell
$w = $env:WORK
dotnet publish dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -r win-arm64 `
  --artifacts-path "$w\shader-dotnet" -o "$w\shader-xlate"
$keys = @(Select-String -Path dm/games/0400486024DEC000/shader_map.tsv -SimpleMatch "`t/Layout2Shader/" |
  ForEach-Object { $_.Line.Split("`t")[3] }) + @('29e3c9dd92b8a571', '8f9728d9b2b88600', 'a5b1bd74e47458d7')
uv run --no-project --python 3.13 --with zstandard==0.25.0 python dm/windows/tools/win_nvn_shaders.py `
  --romfs "$env:DM_ROMFS" --xlate "$w\shader-xlate\dmsh_xlate.exe" --out "$w\vk-shaders-startup" @keys
```

The output directory must not exist; to rebuild, choose a fresh directory and update `DM_VK_SHADERS`. Keep the `.spv` modules, `.vkr` runtime reflection, and `graphics.vkc` capability manifest together. This command selects 148 individual shaders, not every pipeline the game may need. Add keys reported missing at draw submission to the same explicit list and regenerate offline, not at run time.

For the title movie, keep the same graphics keys and add `--ndve-title`. Have `spirv-dis.exe` and `spirv-as.exe` beside `spirv-val.exe` and `spirv-opt.exe`:

```powershell
uv run --no-project --python 3.13 --with zstandard==0.25.0 python dm/windows/tools/win_nvn_shaders.py `
  --romfs "$env:DM_ROMFS" --xlate "$w\shader-xlate\dmsh_xlate.exe" --ndve-title --out "$w\vk-shaders-title" @keys
$env:DM_VK_SHADERS = "$w\vk-shaders-title"
```

This selects 159 modules: the 148 graphics modules plus the 11 distinct compute shaders from the 14 original dispatches of the first Rouleau model. The builder gets the keys from [`nvn_ndve.inc`](../dm/gfx/nvn/nvn_ndve.inc) and verifies their archive through the v131072 shader map. It keeps the actual color conversion, convolutions and resampler. The Windows-only SPIR-V pass rounds the HMMA accumulator after eight products, routes the known tensor addresses through the dispatch's ordinary storage buffers, and types the three surface writes as RGBA8, RGB10A2 and RGBA8. It does not change the Metal path.

Keep `compute.vkc` with `graphics.vkc` in the title library. Replay needs a 32-lane compute subgroup, and the `.vkr` metadata records each module's translated local size and workgroup memory, including HMMA scratch. `spirv-val` checks each module against Vulkan 1.3; the boot and frame capture still test the game's bindings, pipeline states and output.

The builder uses the stock translator. Its Windows-only binary SPIR-V pass declares the capabilities of formatless storage images actually read or written, folds dynamic integer-fetch offsets into the spatial coordinates without moving array layers, and guards raw memory helpers before any write logging. The guard relies on 16-byte-aligned live pool endpoints and Vulkan address deltas and keeps the translator's original zero/no-write behavior on a miss. These changes do not require rebuilding the Darwin translator or Metal shader libraries.

For the opening gameplay task/mesh pairs, pass `--manifest` a capture slice with the `M` pipeline records and `--base` the checked gameplay library. Keep the resulting `mesh.vkc` beside the other manifests. The Windows builder wraps the original Ampere instructions as Vulkan `TaskEXT` and `MeshEXT`, exports the guest mesh indices and attributes, and passes the original task payload to child mesh groups. Only the draw's launch stage takes the `firstTask` push constant. The reached foliage task and its mesh use the checked, read-only guest pool map.

To upgrade a checked capture library that contains dynamic sampled handles, use `--compact-bindless --base <library> --out <new-library>`. The shader keeps the guest texture and sampler ID split and uses checked maps into separate Vulkan view arrays. The library needs descriptor indexing and partially bound arrays; the map for an executed missing ID reports a GPU fault instead of selecting a different descriptor.

For captured classic vertex and fragment shaders that only read raw global memory, add `--graphics-gmem` to the offline build. The checked sidecars opt into Vulkan buffer device addresses and reuse the same live NVN pool map; classic global stores remain unsupported. From the checkout, select the matching gameplay library with `DM_VK_SHADERS`, then run the windowed command above and choose **New Game**. Check the actual submissions:

```bat
findstr /c:"checked VS global memory completed" /c:"checked PS global memory completed" /c:"checked global memory:" "%BOOT_LOG%"
```

The stage/key lines report completed Vulkan submissions, not merely shader loads. The memory line distinguishes live ranges from the coherent backing buffers they share and from bytes copied to staging. Set `DM_VK_GMEM_STATS=1` to see the counts on every checked draw. The GPU fixture below reads and writes two disjoint spans of one host allocation through raw pointers and an ordinary SSBO. Protecting or decommitting the intervening page prevents sharing; recommitting it permits sharing again. The guest cannot read or write the hole in any phase.

`m4_shader_smoke.exe --shading-rate` needs no title files. On N1X it reads back the fragment size for a mixed 1×1/2×2 rate image, changed palettes, a null rate image, and two primitives selecting different tables in one draw. To rebuild affected classic vertex stages from an older library, add `--retranslate-vertices --manifest <keys> --base <old> --out <new>` to the original shader-build options. The translated shader forwards the original NVN VPRS output to a Vulkan viewport index only in per-primitive palette mode; each replicated viewport uses identical geometry.

#### Repeat the Vulkan replay fixtures

After `build_host.cmd`, in the ARM64 build scratch directory, set `DM_VK_SHADERS` to the offline library and run [the smoke](../dm/windows/tools/m4_shader_smoke.c):

```bat
cl /nologo /std:c11 /O2 /W4 /WX /I "%REPO%\dm\third_party\vulkan-headers\include" "%SRC%\m4_shader_smoke.c" m4_draw.obj m4_window.obj m4_shader.obj m4_pipeline.obj /link User32.lib
m4_shader_smoke.exe "%DM_VK_SHADERS%"
```

The smoke creates a Vulkan pipeline from the real Layout2 variation 67 vertex/fragment pair, then checks GPU readback with synthetic shaders. Indexed draws check sampled color (including layer one of a 2D array), depth and untouched pixels. Two compute dispatches check sampled and storage images, uniform and storage texel buffers, and SSBO writes; the second consumes the first's output. The separate `WindowsHost.test_native_headless_present` fixture exercises acquire/present ordering; set `DM_WINARM64_M4_WSI=1` and unset `DM_VK_SHADERS` before running `WindowsHost` from the checkout to exercise the three real Vulkan presents as well as the logical headless path.

For sampled graphics textures, the smoke also binds the same depth view with two different comparison samplers in one draw. It checks readback after changing backing bytes, view swizzles and sampler state independently. Registered views can skip a new upload only while their layout and encoded bytes still match; compute retains its eager planes for alias checks.

The smoke also alternates unsigned `RG16`, signed `RG16` and float sampled views at one fixed binding. It checks the raw register result, not a normalized colour approximation. Attachment readback covers independent depth/stencil changes, masked render targets, and zero coverage while vertex storage writes and precise queries still execute.

To check raw global memory independently of the game and mesh shaders, generate the compute fixture with the same Windows SPIR-V legalization pass used for the gameplay library:

```bat
python "%SRC%\win_gmem_smoke_shader.py" --xlate "%DMSH_XLATE%" --spirv-val "%SPIRV_VAL%" --out "%WORK%\vk-gmem-smoke"
m4_shader_smoke.exe --gmem "%WORK%\vk-gmem-smoke"
```

The fixture negotiates the generated `compute.vkc` capabilities, then checks translated scalar, vector, byte, and halfword accesses against guest readback. It covers disjoint and overlapping ranges, the exclusive pool end, and the rule that a rejected misaligned store must not mark the write log.

`--compute-gmem --base <old> --out <new>` upgrades previously checked read-only compute shaders with the staged-view alias guard. Reapplying `--compact-bindless` to a compact module preserves the existing sampler map even though a compact sampler table still has 4,096 entries. If an older build produced duplicate maps, start from the preceding valid library, not the duplicate.

For the captured Ocarina BC4 encoder, select `a8c283db8ac41d16` and `956eede855c3f379` when building an opening-gameplay library. If reusing an older `--base`, explicitly select the fragment key to refresh its output from the recorded `RGBA16Uint` pipeline format. Run `m4_shader_smoke.exe --bc4 "%DM_VK_SHADERS%"` to check the real pair's BC4 endpoint and selector bytes and confirm that it samples distinct volume slices.

For a windowed run, set `DM_CAPTURE_DIR` to an existing, writable directory reserved for that run. The Windows Vulkan host writes `m4-latest.ppm` on accepted present 1 and every 60th accepted present thereafter, replacing the file atomically. Move the file aside to capture the next accepted present without waiting for the periodic snapshot. It stores top-down RGB from the guest upload before any swapchain scaling, with the presentation sequence and source byte order (RGBA8 or BGRA8) in the PPM comment and the upload dimensions in its header. This works independently of desktop occlusion. Unset the variable to disable capture.

**Exit evidence:** inspect the captured menu, not just black startup frames, a successful present or a fixture. For subsequent gameplay bring-up, keep the title screen as the known-good starting point.

### M5 — Parity with the Mac

- The desktop-GL games through `Zink` instead of a Metal GL layer; movies through FFmpeg or Media Foundation; code-writing cheats through a double-mapped section; a front end reading the existing launcher schema.
- Profile memory keys re-derived for the Windows block rather than inherited from a phone.
- **Exit:** the feature tables in `docs/development.md` read the same for both platforms, and no `[RED]` line is Windows-specific without a note.

### M6 — Continuous

- `scripts/regress.py check --games` on the games it can run, with the report naming the games and why ([dev loop](agents/06-dev-loop.md)).
- Every general mechanism found during the port becomes an entry in its `docs/reference/<subsystem>.md`, with the platform it was found on, and `dm/tools/docs_symptoms.py` regenerates the index. Game-specific facts go into the game's note, in the section they belong to.
- A platform column would be needed in the games table before any row could claim Windows support.

## What would falsify this plan

Revisit the pool design if import cannot preserve guest mapping lifetimes or shader access at game scale; use staged buffers as a comparison rather than assuming a D3D12 switch will fix it. Revisit the scheduling design if measured guest handoff and spin-wait latency prevent a stable frame loop. Neither is resolved by the M0 transfer probe.
