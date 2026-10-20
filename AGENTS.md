# AGENTS.md: working on the `intel-support` branch

Notes for agents (and people) continuing this work. Upstream (`origin`, Castlesea) was developed and tested on a **Ryzen 7 7800X3D + Radeon RX 9070 XT (16 GB)**. This branch makes the port run on an **Intel i7-12650H (hybrid, no SSE4a) + RTX 3050 Ti Laptop (4 GB)**, 32 GB RAM, Windows 11. Read `README.md` and `pc-port/ARCHITECTURE.md` for how the port itself works.

## Rules

- **Never commit or push the game.** `CUSA00004/` (the user's PS4 dump) is listed in `.git/info/exclude`. Stage files by explicit path, never `git add -A` / `git add .`. Game-derived data (`pc-port/out/`, prepared images, shader caches, saves, logs) is git-ignored and stays local.
- Remotes: `fork` = FBaldo98/infamous-second-son-pc-port (push here), `origin` = Castlesea upstream (do not push). Work happens on `intel-support`; `main` on the fork only has the build-script fixes.
- Commits end with the `Co-Authored-By` line the session asks for.

## Build environment (this machine)

| Need | How it was satisfied |
|---|---|
| VS 2022 | Professional edition. `build.py`, `build_gpu.py`, `build_launcher.py` now pick any edition (Community, Professional, Enterprise, BuildTools). |
| `tools/llvm` | A junction to scoop's LLVM (`%USERPROFILE%\scoop\apps\llvm\current`), **Clang 23** (README says 19). |
| `research/shadPS4-perf` | Cloned from weshayoub92/shadPS4-perf. The pinned commit `584374867d6a` no longer exists upstream; HEAD `153a5aa7` was used. Apply the port patch with `git apply --exclude=externals/toml11 ../../pc-port/third_party/shadPS4-perf-local.patch` (the toml11 hunk only marks the submodule dirty). Then `git submodule update --init --recursive --depth 1`. |
| `build-sdl/` | `cmake -S research/shadPS4-perf/externals/sdl3 -B build-sdl -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF` with clang-cl, then build. |
| `build-opt/` | `cmake -S research/shadPS4-perf -B build-opt -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DHAVE_ENDIAN_H=0`, then `cmake --build build-opt`. `HAVE_ENDIAN_H=0` is required with Clang 21+: its builtin `endian.h` lacks `be32toh` and LibreSSL fails. |
| Game folder | `pc-port/out/game_dir.txt` holds the game path (`build.py` has no `--game`). This dump has no `Project.gp4`; `content_profile.py` now treats that as a complete package. |

All CMake/ninja commands run inside `vcvars64.bat` with `tools\llvm\bin` first on `PATH`. Clang 23 adds `-Wunused-but-set-global`, silenced in `build.py` (the runtime builds with `/WX`).

Build order: `prepare.py --game <dir>`, `link_modules.py --game <dir>`, `build.py` (runtime `iss-probe.exe` + 10 test suites), `build_gpu.py --jobs 8` (renderer `iss-gpu.dll`; a header change recompiles everything), `build_launcher.py`. `scripts/make_red_zone_patches.py` regenerates `gpu/red_zone_patches.cpp` from the shadPS4 sources.

`build_release.py` is **not reproducible here**: it packages binaries from the author's `out/experimental/...` folders and a shader recipe pack made from the author's own shader library. `play_porte.py` / `play_v1_guard.py` point at those folders too.

## Running and debugging

- Run folder: `pc-port/out/run/` = `inFAMOUS Second Son Launcher.exe` + `bin/` (`iss-probe.exe`, `iss-gpu.dll`, `SDL3.dll`) + `assets/fonts/` + `user/` (`launcher.ini` with `game_dir=`, `prepared/`, `saves/`, `shader-cache/`, `logs/<timestamp>/`). After each build, copy the binaries into `run/bin/`. The launcher has no autoplay; the user presses PLAY. It strips `ISS_*` variables from its environment, so tuning needs code or `kGameEnvironment` changes.
- Each session writes `stdout.log`, `stderr.log` and `frames.csv` (per-frame timings). stderr is buffered: lines appear in full only after the game exits.
- Wait for a session: poll `tasklist` for `iss-probe.exe` start and exit.
- **Hangs**: `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe -pv -p <pid> -c "!runaway 3; ~*kn 25; qd"` dumps every thread non-invasively. `iss-gpu.dll` has no PDB: convert `export+offset` to an RVA with `llvm-readobj --coff-exports`, then look the RVA up in `pc-port/out/iss-gpu.map` (preferred load address 0x180000000).
- **Crashes**: `PROBE_EVENT` lines in stdout. Host faults now print `HOST_FAULT_FRAME n module+offset` (unwound with the module's unwind data); symbolize `iss-gpu.dll` frames with the map. The guest image is `user/prepared/boot-linked.bin`: the image is the last `image_size` bytes (u64 at offset 8); guest offsets map 1:1 into it (base 0x10000000 at run time). Capstone (`pip install capstone`, in a scratch venv) disassembles it.
- System DLLs load at the same address in every process until reboot: a host `rip` can be mapped to a module by scanning another process's modules.

## What was wrong on Intel + 4 GB NVIDIA, and the fixes

| Symptom | Cause | Fix (where, toggle) |
|---|---|---|
| Illegal instruction at eboot+0x8938f1 during first load | Guest uses SSE4a `EXTRQ` (Jaguar has it, Intel does not). | Trap emulation `runtime_emulate_sse4a` (runtime.c) as fallback; all 96 sites rewritten before guest code by `runtime_patch_sse4a` (`ISS_SSE4A_PATCH=0` off). |
| Null pointer at eboot+0x1cf8f9 / 0x1cfaaf in the intro | Windows writes the exception frame below `rsp`, over the System V **red zone**. Page-tracking faults (and the SSE4a traps) in guest functions with live red-zone values corrupted them. | shadPS4's static Windows red-zone protection, generated into `gpu/red_zone_patches.cpp`, exported as `iss_gpu_patch_red_zone`, called from `probe.c` before guest code: 6453/6460 accesses in 196 functions protected (`ISS_RED_ZONE_PATCH=0` off). TLS and signal parts of the upstream file are cut (the port has its own). |
| `ErrorOutOfDeviceMemory` (2.4 GB allocation) | Five vertex shaders bind a read-only typed buffer of 1.29 billion 2-byte records ("to the end of memory"); the whole range was made resident. NVIDIA sparse buffers allow only one, device-local memory type, so a system-RAM fallback is impossible (`RESIDENCY_TYPES sysmem=-1`). | `CapReadBufferSize` (vk_rasterizer.cpp) caps read-only bindings at a quarter of the VRAM budget, in all binding paths including native passes (`ISS_READ_BUFFER_CAP_MB`, 0 off). |
| Game killed on a driver-internal exception | The vectored handler treated every access violation as fatal, before DLLs' own SEH. | Exceptions inside loaded modules are passed on (`HOST_EXCEPTION_PASSED`); unhandled ones end in `host_fault` with a stack. |
| Driver crash `nvoglv64.dll+0xeea990` in `vkCmdPushDescriptorSetKHR` / `vkUpdateDescriptorSets` (NativePasses::Record) | Native pass templates kept **image views destroyed seconds earlier**. Templates are validated (`TexturesValid`) on the command processor when a segment executes, *after* the pool thread recorded the draw. NVIDIA's driver reads the view object at the push; AMD's apparently does not. The sampler collector also destroyed samplers immediately; it runs on NVIDIA only (maxSamplerAllocationCount 4000). | Record checks every view against a live-handle registry (`gpu/handle_registry.cpp`, hooks the dispatcher's create/destroy) and sends draws with dead views to the full path (`NATIVE_PASSES_DEAD_VIEW`). Deleted images and collected samplers wait `ISS_IMAGE_GRAVEYARD_TICKS` (4) GC ticks before destruction; image destruction bumps the generations. The push and update calls run under SEH and log `PUSH_DESCRIPTOR_FAULT` with each descriptor marked LIVE/DESTROYED. |
| Freeze with audio still playing | `TouchImage` on an unregistered image (alive in the graveyard) re-linked its freed LRU item; reused, it was linked twice, the LRU list became a cycle and `GarbageCollectImages` looped forever holding the cache lock. | `TouchImage` ignores unregistered images. |
| ~23 FPS in heavy scenes | `ISS_CP_CPU=14` pins the command processor (the frame-rate limiting thread) to logical CPU 14: an **efficiency core** on the i7-12650H. | `IssPinCommandProcessor` (bridge.cpp) moves it to a fastest-class core on hybrid CPUs, or with `ISS_CP_CPU=auto`: 6.6 vs 10.8 ms per 1000 draws, ~34 vs ~23 FPS. |

Also on this branch: `ISS_GC_CRITICAL_SPARE_TICKS` (aggressive texture GC spares recently used images). It never triggered in practice (`spared=0`) and was not the crash fix; it can be removed.

## Diagnostics added (log markers)

`SSE4A_PATCH`, `SSE4A_TRAP`, `SSE4A_EMULATED`, `RED_ZONE`, `RESIDENCY_TYPES`, `RESIDENCY_ALLOC_FAILED` + `MEMORY_HEAP`, `RESIDENCY_SYSMEM`, `HUGE_BUFFER`, `READ_BUFFER_CAP`, `GUEST_FAULT_HANDLED` (first handled fault per guest address), `HOST_EXCEPTION_PASSED`, `PROBE_EVENT host_fault` + `HOST_FAULT_FRAME`, `PUSH_DESCRIPTOR_FAULT`, `NATIVE_PASSES_DEAD_VIEW`, `PORT_TUNING cp_cpu ... -> logical N`.

`ISS_NATIVE_PASSES_RESOLVE=1` logs `NATIVE_PASSES_RESOLVE` every report: why a draw's buffer did not resolve on the engine thread, by guest heap (A/B/C), caller (slot binding/geometry/indirect) and `PeekPublishedArena` reason (`not_resident`, `no_arena`, `mixed_arena`, `no_tick`), with the first 16 failing ranges. It is off by default (hot path).

## Native command processor work (2026-10-20)

- The last full session (`logs/20261020-154425`, 11,272 frames): CP busy 30.0 ms of 34.3 ms per frame, 5,415 draws/frame at ~5.5 µs/draw; 74.6% of frames CP > 20 ms; CP grows 13.3 -> 40.7 ms from <3k to >=7k draws/frame. `recorder_execute` is ~0.1% of CP time; the cost is the translated draws' binds (from `ISS_CP_PATH_CYCLES=1`: `bind_res` 2.1 µs, `draw` 1.75 µs, `bind_tex` 1.4 µs, `bind_vertex` 0.85 µs per call).
- 83% of recording attempts miss `MissBuffer`. `ISS_NATIVE_PASSES_RESOLVE=1` shows the causes: with the default unified heap (B) no draw buffer is in it; misses are heap A 31% / heap C 69%, and 99% of the arena attempts are `not_resident`. The scan/record runs before the command processor has materialized those arenas, and the small streaming buffers are served by staging copies the recorder cannot see, so arenas never become resident for them. Only ~15% of draws record natively.
- Host-importing A or C (`ISS_UNIFIED_RANGE=0x80000000:0x20000000` or `0x200000000:0xB8000000`) makes 63-64% of resolves unified and drops `MissBuffer` to 0.5-4.7%, but is a **net loss on this machine**: vertex/index fetches over PCIe, and C also explodes readback waits. `EnableUnified` now imports the contiguous part of a range (heap C's mapping is 0xB7FF0000 of 0xB8000000, a 64 KB gap), which is what made the C test possible.

  Heavy frames (draws >= 2000), scripted autosave, this machine:

  | unified range | unified resolves | MissBuffer | present | CP | readback stall |
  |---|---|---|---|---|---|
  | default (B, 512 MB) | 0% | 89% | 33.8 ms | 32.8 ms | 22.8 ms |
  | B full (1.2 GB) | 0% | 89% | 33.8 ms | 32.8 ms | 22.8 ms |
  | A (512 MB) | 63% | 12% | 50.2 ms | 20.4 ms | 2.4 ms |
  | C (2.875 GB) | 64% | 4.7% | 78.3 ms | 27.6 ms | 173 ms |

- Conclusion: the next step is an engine-side materialization of the draw's buffers (a staging/arena copy the recorder can bind), not host import. Deferred template drops (`NativeMaterial::RequestDrop`) now keep the material table command-processor-only.
- 5 of 12 scripted sessions crashed at `nvoglv64.dll+0xde3240/0xde3281` at 814-894 frames (world load); one of them used the pre-change DLL, so it is not the drop queue, but the dead-view window is not fully closed. The 15:38 DLL survived the same route in its other runs, so this is intermittent.

## Open work

1. **Verify the dead-view check** (commit 3eb8b94) in a long session: no `PUSH_DESCRIPTOR_FAULT`, no host fault, no freeze.
2. **Performance**: the game is bound by the command processor (frame time tracks `command_processor_ms`; cost per draw is constant, frame time rises with draw count, ~2000 to ~5400 draws per frame). `readback_stall_ms` is also large. This is upstream's next roadmap item.
3. **VRAM on 4 GB**: usage sits ~3.5 GB over a 3.37 GB budget, the texture GC deletes tens of thousands of textures per session (re-uploads). Unverified lead: the 512 MB unified-memory region (memory type 3) may live in VRAM through ReBAR.
4. **Release packaging** without the author's folders: parametrize `build_release.py` on `pc-port/out` binaries; shader recipes are optional (the shader cache now persists between clean sessions and compile time is ~1 s per session).
5. 7 red-zone memory accesses are unrelocatable (left unprotected).
6. Old autosaves from the early crashed sessions are in `pc-port/out/run/user/saves.bak-20261020-1148` (not the cause of any crash; can be deleted).
