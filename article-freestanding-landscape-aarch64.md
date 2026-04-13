# The Real Cost of C++ Exceptions in a Freestanding Kernel: 11 Functions and a Configure Flag

In [a previous article](article-exceptions-rtti-aarch64.md), I documented seven walls I hit while getting C++ exceptions and RTTI to work in a bare-metal aarch64 kernel, linking against GCC's stock libsupc++ and libgcc_eh. The runtime glue I had to write was about 300 lines — mostly stubs for functions like `pthread_mutex_lock`, `secure_getenv`, `fwrite`, and `__cxa_get_globals` that libsupc++ reached for but that don't exist in a kernel.

After publishing, I read libsupc++'s source code more carefully. I found that most of those 300 lines were unnecessary. GCC already has a freestanding build mode for libsupc++ — a set of configure options that have existed for years, documented individually but never assembled into a recipe for kernel exception support. When you use them, the stub surface drops from ~30 functions to 11, and the runtime glue file drops from ~300 lines to ~60.

This article is about what those configure options are, what they do, and what the resulting landscape looks like for a kernel developer who wants modern C++ — exceptions, RTTI, `std::atomic`, `std::expected` — in a freestanding environment on GCC 16.

---

## The Configure Recipe

GCC's `configure` script for libstdc++-v3 accepts these options:

```
--disable-hosted-libstdcxx
--disable-tls
--enable-threads=single
--enable-libstdcxx-static-eh-pool
--with-libstdcxx-eh-pool-obj-count=0
--disable-libstdcxx-verbose
```

Plus, to avoid build failures in unrelated libraries that share headers with libstdc++:

```
--disable-libitm --disable-libsanitizer --disable-libgomp
--disable-libquadmath --disable-libssp --disable-libvtv
--disable-libstdcxx-pch
```

Each option addresses a specific piece of the runtime that a kernel doesn't have:

| Option | What it does | Which wall it removes |
|---|---|---|
| `--disable-hosted-libstdcxx` | Sets `_GLIBCXX_HOSTED=0`. Guards out `secure_getenv`, `fwrite`/`fputs`/`stderr`, the verbose terminate handler, `cp-demangle.c`, and the mutex in eh_alloc. | Walls 1 (most stubs), 8, 9 |
| `--disable-tls` | Undefines `_GLIBCXX_HAVE_TLS`. Makes `eh_globals.cc` use a single `__constinit` global instead of `__thread` TLS. | Wall 5 |
| `--enable-threads=single` | Compiles libgcc without gthread support. The btree FDE registry's version-lock and the DWARF reg-size-table once-init no longer reference pthread. | Wall 1 (pthread stubs) |
| `--enable-libstdcxx-static-eh-pool` + `--with-libstdcxx-eh-pool-obj-count=0` | Sets `USE_POOL=0` in `eh_alloc.cc`. The emergency exception pool object — which normally runs a `.init_array` constructor that calls `malloc` before your kernel's heap is ready — is compiled out entirely. | Wall 3 |
| `--disable-libstdcxx-verbose` | Sets `_GLIBCXX_VERBOSE=0`. The default terminate handler becomes `std::abort` instead of `__verbose_terminate_handler`, which would pull in the C++ name demangler and `fwrite`. | Part of Wall 1 |

None of these options requires patching GCC source. They are all standard `configure` flags, each individually documented, each in the GCC source tree for years. What's new is using them together for kernel exception support — a combination that, as far as I can find, has not been documented or tested end-to-end before.

## What's Left: The 11-Function Surface

After building GCC with these options, I linked my toy kernel against the resulting libsupc++ and libgcc_eh with an **empty** runtime file — no stubs at all — and let the linker report every missing symbol. The complete list:

| Symbol | Who needs it | Why |
|---|---|---|
| `malloc` | libsupc++ | `__cxa_allocate_exception` allocates exception objects |
| `free` | libsupc++ | `__cxa_free_exception` releases them |
| `calloc` | libsupc++ | eh_alloc internal use |
| `abort` | libsupc++ | `std::terminate` default handler |
| `memcpy` | libgcc_eh | DWARF CFI interpreter copies register state during unwind |
| `memmove` | libgcc_eh | Same |
| `memset` | libgcc_eh | Initializes unwind context |
| `strcmp` | libsupc++ | `type_info::operator==` compares mangled type names |
| `strlen` | libgcc_eh | CIE augmentation string parsing |
| `__getauxval` | libgcc | aarch64 LSE/SME CPU feature detection (return 0) |
| `_dl_find_object` | libgcc_eh | Dynamic loader FDE lookup fallback (return -1) |

That's it. 9 standard C functions that any kernel already provides, plus 2 platform-specific one-liners. No pthread. No TLS override. No stdio. No environment probes. No stack protector stubs. No `__cxa_get_globals` override.

The resulting runtime file is about 60 lines of C++. The operator new/delete definitions and the `__cxa_atexit` / `__cxa_pure_virtual` stubs that any freestanding C++ program needs are another 15 lines on top. Everything together fits in a single screen.

## Before and After

| Metric | Hosted libsupc++ (previous article) | Freestanding libsupc++ (this article) |
|---|---|---|
| Runtime glue file | ~300 lines | **~60 lines** |
| External stub symbols | ~28 | **11** |
| `__cxa_get_globals` override needed | yes (16 lines) | **no** |
| pthread stubs | 13 | **0** |
| stdio stubs (fwrite/fputs/stderr) | 6 | **0** |
| Environment stubs (secure_getenv, etc.) | 5 | **0** |
| Stack protector / fortify stubs | 2–9 (distro build) | **0** |
| `.init_array` heap-timing issue | yes | **no** |
| TLS register issue | yes | **no** |

## A Bug We Found Along the Way

`--disable-hosted-libstdcxx` does not imply `--disable-tls`. This is a gap in GCC's freestanding configuration.

The `_GLIBCXX_HAVE_TLS` flag is set by `GCC_CHECK_TLS`, which tests whether the *compiler* supports the `__thread` keyword. On a native aarch64 build, it does — so `_GLIBCXX_HAVE_TLS=1` regardless of whether `_GLIBCXX_HOSTED` is 0 or 1.

In `eh_globals.cc`, `_GLIBCXX_HAVE_TLS` is checked *before* `__GTHREADS` or `_GLIBCXX_HOSTED`:

```cpp
#if _GLIBCXX_HAVE_TLS           // checked first
  static __thread ...;           // Path A: reads TPIDR_EL0
#else
  #if __GTHREADS                 // checked second
    ...                          // Path B: pthread_key
  #else
    __constinit ...;             // Path C: single global ← what we want
  #endif
#endif
```

So even in a freestanding build, `eh_globals.o` reads `TPIDR_EL0` — a thread pointer register that a kernel hasn't set up. The result is a data abort on the first `__cxa_end_catch`, reading from a garbage address.

Adding `--disable-tls` to the configure fixes it. Whether `--disable-hosted-libstdcxx` should imply `--disable-tls` is a question for the GCC maintainers. The freestanding path in `eh_globals.cc` (Path C) is well-written and works perfectly — it's just never reached because `_GLIBCXX_HAVE_TLS` takes priority.

We found this by installing a synchronous exception vector table (`VBAR_EL1`) in our boot code. The vector table printed the faulting address (`ELR_EL1`), the exception syndrome (`ESR_EL1 = 0x96000004` — data abort, translation fault level 0, read), and the target address (`FAR_EL1 = 0x00800080008000d0` — garbage from uninitialized TPIDR_EL0). Without the vector table, the symptom was a silent hang after the first catch block, indistinguishable from any other fault.

## What C++26 Gives a Freestanding Kernel

With the GCC freestanding build, I also tested which C++23/26 standard library headers actually compile and work. The method was simple: `#include` each one individually with `-std=c++2c -ffreestanding -nostdlib -fsyntax-only`, then write a combined usage test for the ones that passed.

**21 of 30 tested headers compile and are usable:**

| Category | Headers |
|---|---|
| Synchronization | `<atomic>` — lock-free CAS, fetch_add, memory ordering, atomic_flag |
| Error handling | `<expected>` — `std::expected<T,E>` with monadic chaining |
| Type-safe containers | `<variant>`, `<tuple>`, `<array>`, `<span>` |
| Bit manipulation | `<bit>` — popcount, countl_zero, bit_ceil |
| Type system | `<concepts>`, `<type_traits>`, `<limits>`, `<compare>` |
| Iteration | `<iterator>`, `<numeric>` |
| Core language support | `<exception>`, `<typeinfo>`, `<new>`, `<initializer_list>`, `<cstdint>`, etc. |

**9 headers fail due to GCC implementation gaps:**

`<optional>`, `<string_view>`, `<algorithm>`, `<ranges>` fail because GCC's C++26 `std::format` integration is pulled in unconditionally (the missing file is `bits/formatfwd.h`). `<functional>`, `<memory>`, `<charconv>`, `<cstring>` fail for similar hosted-dependency reasons. All are fixable GCC bugs — the C++ standard marks these headers as (at least partially) freestanding.

**The most important finding: `<atomic>` works end-to-end.** On aarch64, `std::atomic<int>::fetch_add`, `compare_exchange_strong`, and all memory orderings compile to the `__aarch64_ldadd*` / `__aarch64_cas*` dispatch helpers in libgcc, which we already link. No `libatomic` needed. This means a kernel can use `std::atomic` for all synchronization — spinlocks via `atomic_flag`, lock-free queues via `atomic<T*>`, compare-and-swap for wait-free algorithms — without any additional stubs or libraries.

**`<expected>` coexists with exceptions.** `std::expected<T,E>` is available for hot paths (interrupt handlers, scheduler inner loops) where exceptions are too expensive, while `throw`/`catch` remains available for cold paths (initialization, error recovery). This is not an either-or choice — both mechanisms work in the same binary, in the same kernel.

## What This Means in Practice

A kernel developer targeting aarch64 with GCC 16 has access to:

**Runtime features (7 walls for hosted libsupc++, 4 for freestanding):**
- C++ exceptions with full DWARF stack unwinding
- RTTI (`typeid`, `dynamic_cast`)

**Standard library (no additional stubs beyond the 11):**
- `std::atomic<T>` — all integer widths, all memory orderings
- `std::expected<T,E>` — monadic error handling with `transform` / `and_then`
- `std::variant<Ts...>` — type-safe discriminated unions with `std::visit`
- `std::tuple` — structured returns with structured bindings
- `std::array<T,N>`, `std::span<T>` — bounds-aware containers and views
- `std::popcount`, `std::countl_zero`, etc. — hardware bit operations with standard names
- `std::integral`, `std::same_as`, etc. — template constraints

**Kernel obligations (the irreducible contract):**
- Provide 9 basic C functions: `malloc`, `free`, `calloc`, `abort`, `memcpy`, `memmove`, `memset`, `strcmp`, `strlen`
- Provide 2 platform stubs: `__getauxval` (return 0), `_dl_find_object` (return -1)
- Enable FP/SIMD (`CPACR_EL1.FPEN = 0b11`)
- Enable MMU with Normal memory for kernel RAM
- Add `QUAD(0)` sentinel at end of `.eh_frame` in linker script
- Call `__register_frame(&__eh_frame_start)` once before the first throw
- Build GCC with the six-flag freestanding recipe

That is the complete contract. The gap between "hosted C++ program" and "freestanding kernel with modern C++" is this list and nothing else.

---

## Methodology

GCC 16 trunk (commit date 2026-04-12) built from source with the configure options listed above. Kernel: the same `cppkernel` toy kernel from the previous article — aarch64, QEMU virt, Cortex-A72. Tests: M0 through M4 (boot, global constructors, heap, exceptions, RTTI) all pass.

The C++26 header probe was done by compiling each `#include` individually with `-std=c++2c -ffreestanding -nostdlib -fsyntax-only` inside the same Docker container. Feature usage was verified with a combined test file that exercises `atomic` CAS, `expected` chaining, `variant` visit, `span` subspan, `bit` popcount, `concepts` constraints, and structured bindings — all compiled without errors.

The 11-symbol surface was found by linking the kernel with an empty runtime file (zero stubs) and collecting every `undefined reference` the linker reported. No symbols were added speculatively.

The TLS bug was diagnosed using a synchronous exception vector table (`VBAR_EL1`) that reports `ELR_EL1` (faulting PC), `ESR_EL1` (exception syndrome), and `FAR_EL1` (fault address) on any data abort. Installing this vector table early in the boot sequence is the single most impactful debugging technique for bare-metal aarch64 work.

The full source, development log, GCC configure recipe, and Dockerfile are in the project repository.

Questions or similar findings: toneverdo@gmail.com.
