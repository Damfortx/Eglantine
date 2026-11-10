# Portability fixes (Windows MSVC, Linux aarch64 GCC, macOS arm64 clang)

The Mario Kart World work on `main` was only ever built with clang on Linux x86_64. The CI
builds for the other platforms failed; these are the fixes that made them pass. They do not
change Linux x86_64 code paths. To port them to `mkw-mt-lite` (Eglantine-mtB) and `mkw-c11`
(Eglantine-c11), cherry-pick the commits listed at the end or apply each item by hand where the
file exists on that branch (line numbers drift; search for the quoted code).

## What the compilers reject

- **MSVC** (`src/*/CMakeLists.txt` passes `/we...`): C4456/C4457/C4458/C4459 (a declaration
  hides a local, a parameter, a class member or a global), C4244/C4242 (narrowing conversion,
  also when it happens inside a std template such as `std::pair<u32, T>{size_t, ...}`),
  C4062, C4189, C4265, C4388/C4389, C5038, C5233, C5245. No `pthread.h`, `sched.h`, no
  `__builtin_ia32_*`.
- **GCC 13** (Linux CI, both arches): `-Werror=all -Werror=extra`: `-Wdangling-pointer`,
  `-Wclass-memaccess`.
- **Apple clang**: `-Werror -Wshadow` (field shadowing).

## Fixes

### Shadowing (MSVC C4456-C4459, clang -Wshadow)

| File | What | Fix |
|---|---|---|
| `video_core/engines/kepler_compute.h` | `MtSetDispatchState(const MtDispatchState& state)` hides member `state` | parameter renamed `dispatch` |
| `video_core/mt/gpuparam.h` | `BuiltinsMatch(std::span<const u32> words)` hides member `words` | renamed `record_words` |
| `video_core/mt/gpuparam.h` | local `std::string words` in the macro log | renamed `code_text` |
| `video_core/mt/gpuparam.h` | `Compare(..., words, ..., expected, ...)` hide members `words`/`expected` | renamed `call_words` / `expected_draws` |
| `video_core/mt/list_dump.h` | `Record`: local `std::FILE* const file` hides member `file` | renamed `out` |
| `video_core/renderer_vulkan/vk_mt_pipeline.h` | `CaptureDraw`: `using Maxwell = Maxwell3D::Regs;` hides the global `Vulkan::Maxwell` | alias renamed `MaxwellRegs` (whole function) |
| `video_core/renderer_vulkan/vk_mt_resolve.h` | `Resolver::ResolveOne`: same `using Maxwell` | alias renamed `MaxwellRegs` (whole function) |
| `video_core/renderer_vulkan/vk_mt_resolve.h` | `stream_uniform` lambda parameter `index` and the uniform loop's `const u32 index` hide `Resolver::index` | renamed `ubo_index` (not the `.index`/`->index` member accesses) |
| `video_core/renderer_vulkan/vk_mt_resolve.h` | texture loop: `const TextureCache::MtResolvedView resolved` hides `Nx2DrawPipeline::resolved` | renamed `resolved_view` |
| `video_core/renderer_vulkan/vk_mt_resolve.h` | counters log: inner `std::string fail_text` (RBUF block) hides the outer one | inner renamed `buffer_fail_text` |

### x86 intrinsics and POSIX APIs

- Spin pauses written as `#if defined(__x86_64__) || defined(_M_X64) __builtin_ia32_pause()`
  do not compile on MSVC. Now:

  ```cpp
  #if defined(_MSC_VER) && defined(_M_X64)
      _mm_pause();               // + #include <intrin.h> under #ifdef _MSC_VER
  #elif defined(__x86_64__)
      __builtin_ia32_pause();
  #else
      std::this_thread::yield();
  #endif
  ```

  in `video_core/gpu_split.cpp` (`CpuRelax`), `video_core/mt/resolve_sync.h` (`CpuPause`) and
  `video_core/renderer_vulkan/vk_mt_pipeline.h` (`MTDetail::Pause`).
- `video_core/gpu_split.cpp`: `#include <sched.h>` / `<pthread.h>` only under `#ifdef __linux__`
  (they are only used by `PinThisThread`, which is already Linux-only).
- `video_core/host1x/syncpoint_manager.cpp` (`EDEN_NX2_SYNCPT_DEBUG`): `<pthread.h>` and
  `pthread_getname_np` only on `__linux__ || __APPLE__`; elsewhere the thread name is `"?"`.

### GCC-only diagnostics

- `video_core/mt/selftest_phase5.h`, `BatchPublishStressTest`: GCC 13 (aarch64 and x86_64 CI)
  reports a false `-Wdangling-pointer` for the `close_batch` lambda's by-reference captures
  when inlined into `gpu_split.cpp`. The function is wrapped in
  `#pragma GCC diagnostic push/ignored "-Wdangling-pointer"/pop`, guarded by
  `defined(__GNUC__) && !defined(__clang__)`.
- `video_core/renderer_vulkan/vk_mt_pipeline.h`, `CommitCompute`: `std::memcpy(&header, ...)`
  into the non-trivial `ComputeHeader` (`-Wclass-memaccess`) now copies through
  `static_cast<void*>(&header)`.

### Narrowing conversions (MSVC C4244)

- `video_core/renderer_vulkan/present/lsfg_mipmaps.cpp` (upstream code):
  `{LSFG_MIP_LEVELS, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}` builds a `std::pair<u32, ...>` from a
  `size_t` -> `static_cast<u32>(LSFG_MIP_LEVELS)`.
- `video_core/renderer_vulkan/vk_fsr3_upscaler.cpp`: `frame_parity = static_cast<u32>(q);`.
- `video_core/mt/selftest.h`: `regs[method] = static_cast<u32>(rng());`.
- `core/launch_params.cpp` (upstream code): `p.selected_user = static_cast<int>(user_idx.value());`.

## Checking locally

There is no MSVC here; these approximate it on the Linux build tree (`compile_commands.json`
of a GCC build of the same tree):

- GCC with `-fsyntax-only -Wshadow -Wsign-compare -Wswitch -Wreorder -Wunused-variable` over
  `src/video_core` and the changed files (MSVC C4456-C4459, C4389, C4062, C5038, C4189).
- clang with `-Wshadow-all -Wunused-lambda-capture` (macOS, C5233).
- clang with `-Wno-everything -Wshorten-64-to-32 -Wimplicit-int-conversion -Wfloat-conversion
  -Wsystem-headers` over `src/{video_core,core,audio_core,hid_core}`, attributing warnings in
  system headers to the first `src/` frame of the instantiation notes (C4244 inside std
  templates). Most hits inside libstdc++ (`<regex>`, `uniform_int_distribution`) do not exist
  in the MSVC STL and can be ignored.
- A plain GCC 13 build (`-DCMAKE_CXX_COMPILER=g++`) reproduces the Linux CI job.

## Commits (on `main`)

- `b8e2178c97` ci: portability fixes for MSVC, GCC aarch64 and Apple clang (shadowing, pauses,
  pthread/sched guards, `-Wdangling-pointer`, `-Wclass-memaccess`).
- `1010be1751` ci: explicit narrowing casts for MSVC C4244.

First all-green run: CI run 42823917797 (Windows x86_64, Linux x86_64/aarch64, macOS arm64),
pre-release `branch-main-0d6e4079`.
