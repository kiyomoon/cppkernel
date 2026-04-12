# Running C++ Exceptions and RTTI in a Bare-Metal aarch64 Kernel

I wanted to know whether C++ exceptions could actually work in a kernel.

Every freestanding C++ guide, embedded C++ tutorial, and kernel coding standard I've ever read leads with two flags: `-fno-exceptions` and `-fno-rtti`. They always travel together. The stated reasons cluster into "exceptions are slow," "exceptions are non-deterministic," and "exceptions need too much runtime support for a freestanding environment" — and by association, the same arguments get applied to RTTI, even though RTTI on its own doesn't do stack unwinding. Neither flag survives contact with real systems code. Both get turned off, as a pair, everywhere.

I wasn't interested in the cost arguments in this experiment. The interesting question was the third one: do the runtimes for these features actually need so much environment support that a freestanding kernel can't host them? Or, more simply: on modern GCC, on modern aarch64, what happens if you just... don't disable them?

So I built a toy kernel and tried. The short version: both work. Exceptions took seven subtle walls to get past, most of which turned out to have nothing to do with exceptions themselves — they were CPU bring-up issues that any aarch64 kernel has to solve anyway. Once exceptions were running, RTTI was a single commit: one flag removed, one test added, zero new runtime machinery required. That was the more interesting finding for me: **in this setup, the two flags that embedded guides usually disable together turned out to be much closer to one decision than two separate ones.** C++ exception type matching already requires the RTTI machinery (`type_info`, class hierarchy descriptors, `__dynamic_cast`) to be linked and working. Once that machinery is present, `typeid` and `dynamic_cast` on ordinary polymorphic types come along almost for free.

This article records the seven walls because they were not obvious to me at the start, and because each one is manageable once you know where it lives. The RTTI postscript at the end of the article explains why it came for free here, and why the conventional pairing of `-fno-exceptions -fno-rtti` deserves a more careful explanation on modern toolchains than it usually gets.

---

## The Experiment

`cppkernel` is a toy aarch64 kernel I built in about two days as part of a larger "C++ in kernel space" investigation. It runs on the QEMU virt machine with `-cpu cortex-a72 -m 128M`, is compiled with GCC 14 or later in `-std=c++17` mode (the code uses nothing newer), and links statically against `libsupc++.a` and `libgcc_eh.a` exactly as they ship in the GCC install. Those two libraries are used unmodified — no patches, no forks, no rebuild. The investigation was done on GCC 16 trunk because the surrounding project is exploring C++26 reflection for a later milestone, but stock Ubuntu `g++-14` is sufficient for everything in this article. The work that went into making the kernel *able to use* the runtime is a different story, and it's what the rest of this article is about.

The kernel has six source files:

- `boot.S` — aarch64 entry point (EL1 dispatch, FPU enable, MMU bring-up, BSS zero, `.init_array`, jump to C++)
- `kernel.cpp` — the test driver
- `cxxrt.cpp` — the libc stub surface and one override
- `heap.hpp` — a 512KB bump allocator
- `uart.hpp` — PL011 driver at `0x09000000`
- `linker.ld` — standard section layout

The tests the kernel has to pass are these:

```cpp
// --- M3: exceptions ---

struct KernelError {
    int errno_val;
    const char* msg;
};

[[gnu::noinline]] static void inner_function() {
    uart::puts("  entering inner_function\n");
    throw KernelError{-22, "EINVAL from inner"};
}

[[gnu::noinline]] static void outer_function() {
    uart::puts("  entering outer_function\n");
    inner_function();
}

static void test_exceptions() {
    // Same-frame throw/catch
    try {
        throw KernelError{-1, "direct throw"};
    } catch (const KernelError& e) {
        print_errno(e);
    }

    // Throw across two real stack frames
    try {
        outer_function();
    } catch (const KernelError& e) {
        print_errno_and_msg(e);
    }
}

// --- M4: RTTI, reusing an M2 polymorphic hierarchy ---

struct FileSystem { virtual ~FileSystem() = default;
                    virtual const char* type_name() = 0; };
struct RamFS  : FileSystem { const char* type_name() override { return "ramfs";  } };
struct ProcFS : FileSystem { const char* type_name() override { return "procfs"; } };

static void test_rtti() {
    FileSystem* fs1 = new RamFS();
    FileSystem* fs2 = new ProcFS();

    // typeid on polymorphic reference — must reflect the runtime type.
    check(typeid(*fs1).name() == mangled("RamFS"));
    check(typeid(*fs2).name() == mangled("ProcFS"));

    // Downcasts: success when the dynamic type matches, nullptr otherwise.
    check(dynamic_cast<RamFS*>(fs1)  != nullptr);
    check(dynamic_cast<ProcFS*>(fs1) == nullptr);
    check(dynamic_cast<ProcFS*>(fs2) != nullptr);

    check(typeid(*fs1) != typeid(*fs2));
}
```

The exception tests need their two throws caught and their fields read. The RTTI tests need the runtime type information for `RamFS` and `ProcFS` to survive linking, `typeid` to return the correct mangled names (`5RamFS` / `6ProcFS` in Itanium mangling), and `dynamic_cast` to correctly succeed or return nullptr based on the actual dynamic type.

That's the whole test surface. Two throws, two catches, stack unwinding across non-inlined frames, plus a handful of type-query operations. The reason this is interesting is that it's an end-to-end check of a very long chain: `throw` → `__cxa_allocate_exception` → `__cxa_throw` → `_Unwind_RaiseException` → two-phase Itanium ABI search → personality routine → `__cxa_begin_catch` → catch block → `__cxa_end_catch`, plus `typeid` → `std::type_info::name()` and `dynamic_cast` → `__dynamic_cast` → class hierarchy walk. Every link in those chains lives inside unmodified libsupc++ or libgcc_eh. All the kernel has to do is bring the CPU up into a state those libraries can assume — FP/SIMD enabled, MMU on with RAM mapped as Normal memory, a few heap and linker details — and otherwise stay out of the runtime's way. No scheduler, no interrupts, no filesystem, no userspace. Just enough CPU bring-up for the runtime to run.

That turned out to be the hard part.

## The Seven Walls

Each wall hid the next one. Every time I got past one, the next symptom looked like "same problem, but deeper," and I had to stare at disassembly for a while to realize it was actually something new. I'll describe them in the order I hit them, with the symptom first, because that's what future-me will see if I retry this on a different platform.

### Wall 1 — The stub list is larger than the tutorials say

Every freestanding C++ tutorial tells you you need `operator new`, `operator delete`, `__cxa_atexit`, `__cxa_pure_virtual`, and `__dso_handle`. These are enough for constructors and virtual dispatch. They are not even close to enough for exceptions.

The actual list of symbols libsupc++ and libgcc_eh want resolved at link time, for the test above, is about thirty functions. Grouped by category:

| Category | Functions |
|---|---|
| allocation | `malloc`, `free`, `calloc`, `realloc` |
| mem/str | `memcpy`, `memset`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strchr`, `strncmp` |
| panic/io (unreached if terminate handler is overridden) | `abort`, `sprintf`, `fputs`, `fputc`, `fwrite`, `stderr` |
| env probes | `secure_getenv`, `getenv`, `__isoc23_strtoul`, `__getauxval`, `getauxval` |
| pthread (no-op, single-threaded) | `pthread_mutex_*`, `pthread_key_*`, `pthread_cond_*`, `pthread_once`, `pthread_getspecific`, `pthread_setspecific` |
| loader | `_dl_find_object` |

None of it is exotic. `malloc` forwards to the bump allocator. `free` is a no-op. The pthread functions are all no-ops because we're single-threaded. The env-reading functions return null, which causes libsupc++'s exception pool to use its compiled-in default size. `_dl_find_object` returns failure, which causes libgcc's unwinder to fall back to the static `__register_frame` registry instead of the glibc dynamic-loader path.

The interesting one is `strcmp`. Even compiled with `-fno-rtti`, C++ exception type matching still goes through `type_info::operator==`, which compares the mangled type names with `strcmp`. If you remove `strcmp` from the stub list, every `catch` block becomes a link-time error. `-fno-rtti` does not strip the type info for *thrown* types; it only strips `typeid` and `dynamic_cast` on non-thrown paths.

Total: ~150 lines of stubs. Most of them one-liners.

### Wall 2 — Do not `--whole-archive` libgcc_eh

Older guides for linking libgcc_eh into freestanding binaries tell you:

```
-Wl,--whole-archive -lgcc_eh -Wl,--no-whole-archive
```

The reasoning was that parts of the unwinder's frame-lookup code used to be referenced only indirectly through `.eh_frame` data, and the linker's dead-code elimination would drop them if you didn't force-include.

On modern GCC (14 and later), this is no longer true. `__cxa_throw` in libsupc++ has a direct symbol reference to `_Unwind_RaiseException`, which pulls the entire DWARF unwinder through normal archive resolution. Plain `-lgcc_eh` is sufficient.

`--whole-archive` is actively harmful in freestanding builds. It drags in objects that have no reason to exist in a kernel — `emutls.o` (for `__thread` TLS emulation), `heap-trampoline.o` (for nested functions), `__aarch64_have_sme.o` (Scalable Matrix Extension detection), `lse-init.o` (Large System Extensions atomic detection), `unwind-dw2-fde-dip.o` (dynamic loader FDE lookup). Each of those drags in more undefined symbols: `mmap`, `pthread_cond_*`, `_dl_find_object`, `__getauxval`. The chain is long, and none of it is load-bearing for actual exception handling.

Drop `--whole-archive`. Link with plain `-lsupc++ -lgcc_eh -lgcc`. This is the first piece of freestanding-C++ lore that turned out to be wrong on modern GCC.

### Wall 3 — libsupc++ runs a static constructor before your `main`

`libsupc++.a(eh_alloc.o)` contains a file-scope static: the exception memory pool. Its constructor runs from `.init_array`, which every freestanding kernel traverses before entering `main`. The constructor reads `GLIBCXX_EXCEPTION_POOL_SIZE` (null → default size), calls `malloc(pool_size)`, and `memset(pool, 0, pool_size)`.

My original design had `heap::init()` called from inside `kernel_main`. That put heap setup too late: the libsupc++ constructor wanted to `malloc` during `.init_array`, the bump pointer was still zero, and the subsequent `memset` wrote to low memory addresses and hung the kernel.

The fix is one line: `heap::alloc` does its own lazy init, `if (!next) next = __heap_start;`. Calling `heap::init()` later from `kernel_main` is then redundant, and if you're not careful it *resets* the bump pointer, overwriting the exception pool that libsupc++ already allocated. The safe version is idempotent — only initialize if not initialized yet.

Generalization: in freestanding C++ you cannot assume your main-equivalent runs before any allocation. Static constructors from linked archives reach for `malloc` during `.init_array`. Plan your heap ordering accordingly.

### Wall 4 — `.eh_frame` needs a zero sentinel

`__register_frame(begin)` in libgcc walks contiguous FDE (Frame Descriptor Entry) records, advancing by each record's length field, until it hits a CIE with length zero. That zero terminator is normally contributed by `crtend.o`'s tiny `.eh_frame` fragment in hosted builds. Freestanding doesn't link crtend.

Without the sentinel, `__register_frame` walks off the end of `.eh_frame` into whatever happens to follow it in memory, interprets garbage bytes as FDE lengths, and either loops forever or faults.

Fix: add `QUAD(0)` at the end of the `.eh_frame` output section in `linker.ld`.

```
.eh_frame : {
    __eh_frame_start = .;
    KEEP(*(.eh_frame))
    QUAD(0)
    __eh_frame_end = .;
}
```

Two words. Probably the cleanest fix in the whole investigation, and — having checked a few freestanding C++ tutorials — not something any of them mention.

### Wall 5 — `__cxa_get_globals` uses thread-local storage

This one took longer to figure out than all the others combined, because the symptom was so far from the cause.

When libstdc++-v3 is built with `_GLIBCXX_HAVE_TLS` set (the default on aarch64 glibc targets), its implementation of the per-thread exception state holder is:

```cpp
static __thread __cxa_eh_globals global;
__cxa_eh_globals* __cxa_get_globals() { return &global; }
```

On aarch64, `__thread` compiles to a load from `TPIDR_EL0` plus a static offset. `TPIDR_EL0` is the user-mode thread pointer register. On Linux it's programmed by the kernel before each thread resumes. Our kernel never programs it — it reads zero or garbage. The subsequent dereference faults.

If you haven't installed an exception vector table yet (which we hadn't), the fault silently hangs the CPU. The symptom is: `__cxa_throw` enters, makes one call, and nothing prints after that.

The fix is to define `__cxa_get_globals` and `__cxa_get_globals_fast` in the kernel's own runtime TU:

```cpp
static struct { void* caughtExceptions;
                unsigned int uncaughtExceptions; } kernel_eh_globals;

extern "C" void* __cxa_get_globals()      noexcept { return &kernel_eh_globals; }
extern "C" void* __cxa_get_globals_fast() noexcept { return &kernel_eh_globals; }
```

The interesting part is *how* this override works. The linker processes `.o` files left-to-right and only pulls objects from `.a` archives to satisfy *unresolved* references. If our `cxxrt.o` defines `__cxa_get_globals` before `libsupc++.a` is processed, then when the linker sees libsupc++'s `eh_throw.o` (which references `__cxa_get_globals`), the reference is already satisfied, and `eh_globals.o` never gets pulled from the archive at all. The entire TLS dependency chain disappears.

No `--wrap`. No patching of libsupc++. No rebuilding anything. Just "define the symbol earlier on the link line and the archive member goes dark." I found this override pattern underused in general; it's a clean way to replace a specific TU from a static archive.

Single-threaded kernel, so one static global is fine. The struct layout has to match libsupc++'s `__cxa_eh_globals` — one `void*`, one `unsigned int`, no `propagatingExceptions` because we're not `__ARM_EABI_UNWINDER__`.

### Wall 6 — FP/SIMD traps by default, and GCC inlines memset as NEON

This is the wall that cost me the most debugging time, because its symptom looked identical to Wall 5 at first glance: the kernel hung somewhere between `throw` and the `catch` block, with no output.

The clue was in the disassembly of `__cxa_allocate_exception`. It looks like this on aarch64:

```asm
400832d4: bl  malloc               // ret = malloc(header_size + n)
400832d8: mov x1, x0
400832dc: cbz x0, ...               // if null, pool fallback
400832e0: movi v31.4s, #0x0         // NEON: v31 = 0
400832e4: add x0, x1, #0x80         // return = ret + 128
400832e8: stp q31, q31, [x1]        // NEON: store 32 bytes
400832ec: stp q31, q31, [x1, #32]   //   32 more
400832f0: stp q31, q31, [x1, #64]   //   32 more
400832f4: stp q31, q31, [x1, #96]   //   32 more = 128 total
400832f8: ...                       // return
```

The `memset(header, 0, 128)` inside `__cxa_allocate_exception` got inlined as four NEON 128-bit pair stores. `stp q31, q31, [addr]` writes 32 bytes per instruction from the NEON register bank.

On aarch64, `CPACR_EL1.FPEN` is `0b00` on reset. Any FP or SIMD instruction at EL1 or EL0 traps to EL1. Without a vector table installed (`VBAR_EL1 = 0` or garbage), the trap has nowhere to go, and the CPU loops indefinitely.

The reason M0, M1, and M2 ran fine without hitting this was pure coincidence: none of their generated code happened to use NEON. GCC picks NEON memset when the buffer is large enough and aligned enough; M0/M1/M2 never produced such a pattern. The first NEON instruction in the actual execution path of the kernel was inlined into `__cxa_allocate_exception`. The first `throw` statement in the kernel was the first thing that hit it.

Fix: in `boot.S`, before anything else runs (including `.init_array`), set `CPACR_EL1.FPEN = 0b11` and follow with `isb`:

```asm
mrs x0, cpacr_el1
orr x0, x0, #(3 << 20)
msr cpacr_el1, x0
isb
```

Six instructions. The catch: this has to happen before `.init_array`, because libsupc++'s exception pool static constructor also does a memset.

### Wall 7 — MMU off means "Device memory," which rejects unaligned SIMD

With Wall 6 fixed, `__cxa_allocate_exception` returned, `__cxa_throw` ran, `_Unwind_RaiseException` ran, and `uw_init_context_1` ran — but then execution stopped inside `uw_frame_state_for`, *before* its first call to `_Unwind_Find_FDE`.

The clue this time was that `__cxa_allocate_exception`'s inlined NEON memset had worked. It wrote to an address returned by `malloc`, which our bump allocator aligns to 16 bytes, so the NEON pair stores hit a 16-aligned target. `uw_frame_state_for`'s inlined memset writes to a local `_Unwind_FrameState fs` placed on the stack. GCC put it at `sp + 0x48`. That's 8-byte aligned but **not** 16-byte aligned.

An 8-aligned NEON pair store is where ARMv8-A memory type rules matter:

- On **Normal** memory, unaligned loads and stores are allowed unless `SCTLR_EL1.A = 1`.
- On **Device** memory, unaligned SIMD pair stores are not allowed. They take an alignment fault.

When the MMU is disabled, all data accesses are treated as Device-nGnRnE by default. The `stp q31, q31, [fs, #800]` from `uw_frame_state_for` hit Device memory at an 8-aligned address, faulted, and — still no vector table — hung.

The fix is to bring up a minimum MMU configuration:

- **MAIR_EL1**: two attributes, `0x00` (Device-nGnRnE) in slot 0, `0x44` (Normal Inner/Outer Non-Cacheable) in slot 1.
- **Page table**: one L1 table with four 1GB block entries. Entry 0 maps `0–1GB` to Device memory (that's where the UART lives at `0x09000000`). Entries 1, 2, 3 map `1–4GB` to Normal NC (kernel RAM starts at `0x40000000`).
- **TCR_EL1**: `T0SZ=32` (4GB virtual address space), `EPD1=1` (disable TTBR1 walks), non-cacheable page table walks.
- **SCTLR_EL1.M = 1** (enable the MMU).

Notably, I did *not* enable the data or instruction caches (`SCTLR_EL1.C` and `I`). Non-cacheable Normal memory is enough to make unaligned SIMD legal, and avoiding caches avoids a whole category of cache-coherence bring-up work. For a toy kernel, this is a fine tradeoff.

One sub-gotcha: the page table lives in `.bss` for 4KB alignment and zero initialization. The BSS zeroing loop has to run *before* MMU setup, not after. In my first attempt I had the order backwards, and zeroing BSS after enabling the MMU wiped the translation table out from under the running CPU — the next instruction fetch missed the TLB, the walk read zeros, and the kernel hung the instant BSS was cleared. Moving the BSS clear ahead of the MMU block fixed it.

## Dependencies Between Walls

Walls 5, 6, and 7 are worth naming together because they mask each other.

When I was fighting Wall 5 (the TLS issue), I spent a while believing that `__register_frame` itself had a bug — `register_pc_range_for_object` → `classify_object_over_fdes` → hang. I even worked around it with `--wrap=__register_frame` to no-op. The workaround moved the hang into Wall 6. I fixed Wall 6 (FP/SIMD), and the hang moved into Wall 7. I fixed Wall 7 (MMU), and everything downstream of Wall 7 — including the `__register_frame` code path I had worked around earlier — ran cleanly on the first try.

**Walls 5, 6, and 7 had almost nothing to do with `__register_frame`**. They had to do with NEON instructions in Device memory and unset TLS registers. I only *thought* `__register_frame` was the problem because the hang appeared inside its call tree, and without a vector table I couldn't tell the difference between "bug inside this function" and "fault on the next NEON instruction this function issues."

The generalization:

> In a kernel without an exception vector table, every CPU fault looks identical — a silent hang. You cannot tell a NEON trap from an alignment fault from a page fault. The only debugging tool is "add UART prints between every two instructions and bisect."

The next feature bring-up for this kernel will install a minimal synchronous-exception handler at `VBAR_EL1` *first*, before any other work, so that the next hang shows up as `fault at ELR=0x... ESR=0x...` instead of silence. Doing that from the beginning would have saved most of the time I spent on Walls 5, 6, and 7.

## What It Looks Like When It Works

After all seven walls were fixed, the nested throw test produced this trace. I've annotated it because the `printf`-style trace I was using during debugging captures every stub call, which turns out to be a surprisingly beautiful view of the Itanium C++ ABI two-phase unwinder in action:

```
entering outer_function
entering inner_function
  malloc(0x90) → 0x400b3360                        exception header + KernelError
  __cxa_throw(obj, tinfo, dtor)
    __cxa_get_globals()                            → our single-global override
    _Unwind_RaiseException()
      uw_init_context_1()
        memset(context, 0, 960)
        _Unwind_Find_FDE(pc=inside libgcc)         find first frame's CFI
      [phase 1: search for handler]
        _Unwind_Find_FDE(0x40082f33)               __cxa_throw frame
        _Unwind_Find_FDE(0x40084f0f)               ...
        _Unwind_Find_FDE(0x40082e57)               ...
        _Unwind_Find_FDE(0x40081273)               inner_function
        _Unwind_Find_FDE(0x4008128b)               outer_function
        _Unwind_Find_FDE(0x400810bb)               kernel_main (HANDLER FOUND)
      [phase 2: unwind + install]
        _Unwind_Find_FDE(0x40082f33)               (same sequence, executing
        _Unwind_Find_FDE(0x40084f0f)                destructors and restoring
        _Unwind_Find_FDE(0x40082e57)                callee-saved registers
        _Unwind_Find_FDE(0x40081273)                along the way)
        _Unwind_Find_FDE(0x4008128b)
        _Unwind_Find_FDE(0x400810bb)
      __cxa_begin_catch()
        __cxa_get_globals()
  caught nested: errno=0x16 msg=EINVAL from inner
      __cxa_end_catch()
```

Two phases, 13 FDE lookups, eight real stack frames. Everything between "malloc" and "caught nested" runs inside unmodified libsupc++ and libgcc_eh, talking only to my kernel's stubs.

The boring part is the point: a freestanding kernel doing C++ exception handling looks exactly like a hosted process doing C++ exception handling. Same library code, same call sequence, same result. Nothing clever.

## A Postscript on RTTI

After M3 passed I expected RTTI to be its own adventure. `-fno-rtti` is the companion of `-fno-exceptions` in every embedded C++ guide, and the usual rationale is a mirror of the exception rationale: the runtime needs `type_info`, class hierarchy descriptors, `__dynamic_cast`, maybe demangling infrastructure. All of that is supposed to bring its own set of link-time demands and runtime assumptions.

The work it actually took to enable RTTI was this:

- Remove `-fno-rtti` from the Makefile.
- Add `#include <typeinfo>` to `kernel.cpp`.
- Add a test function using `typeid(*fs).name()` and `dynamic_cast<RamFS*>(fs)` on the same `FileSystem`/`RamFS`/`ProcFS` hierarchy that M2 had used to test vtables.
- Rebuild.
- Run.

The test passed on the first try. Zero new walls. Zero new stubs. Zero changes to `boot.S`, the linker script, the heap, or the runtime glue. The link step produced no new undefined references. Every piece of libsupc++ machinery that RTTI needs — `__cxxabiv1::__class_type_info`, `__cxxabiv1::__si_class_type_info`, `__dynamic_cast`, `type_info::name()` — was already in the binary, pulled in as a dependency of the M3 exception work.

The reason, once I sat with it for a minute, is structural. C++ exception type matching is already RTTI: when a `catch(KernelError&)` clause runs, the runtime has to ask "is the thrown object's dynamic type compatible with `KernelError`?" — and that question is answered by exactly the same `type_info::operator==` and class hierarchy walk that `dynamic_cast` uses. In GCC's implementation, they are the same code. libsupc++'s `__do_catch` member on `__class_type_info` shares the same machinery as `__do_dyncast`. The `strcmp` call on mangled type names in the catch path that I discovered during M3 debugging is *the same strcmp* that `dynamic_cast` uses. In this environment, once one of these two features is working, the other is nearly free because the runtime support is already there.

This suggests the conventional pairing of `-fno-exceptions -fno-rtti` in embedded C++ guides is broader than this GCC/aarch64 setup justifies. You can still disable both for size or policy reasons. But if exceptions are already on, RTTI stops looking like a separate runtime project and starts looking more like a flag that stayed off out of habit: exceptions need RTTI, so `-fno-rtti -fexceptions` is either a compilation error or quietly depends on the same machinery it claims to avoid. (GCC documents this explicitly: "The mechanism used to implement exceptions requires the type information system to work, so disabling RTTI will break exception handling.")

What *is* a historically-valid reason to disable both: the `__verbose_terminate_handler` path that the C++ runtime installs by default pulls in `cp-demangle.o`, which is a substantial chunk of code dedicated to turning mangled names into readable form for a programmer to see. That code is dead weight in a kernel. But as described in Wall 5's cleanup, installing your own terminate handler via `std::set_terminate` prevents `cp-demangle.o` from ever being pulled from libsupc++.a, because nothing else references it. The size cost of "RTTI enabled" in a kernel that installs its own terminate handler is small — typeinfo objects for the classes you actually use, plus a handful of `__class_type_info` vtables, plus the `__dynamic_cast` routine. A few KB at most.

I don't want to over-claim — this is a toy kernel with three polymorphic classes, and a real kernel with hundreds of polymorphic types would have proportionally more typeinfo data. But the point stands: the marginal cost of turning RTTI on, *given that exceptions are already on*, is negligible. If your default policy is "disable both," then it's still worth noticing that the first decision (disable exceptions) makes the second decision (disable RTTI) much larger than it needs to be. Flipping the order of that reasoning — "if I'm turning exceptions on anyway, RTTI is already there" — changes the tradeoff considerably.

## What I Did Not Do

This experiment established one fact and nothing else. A few things that are explicitly out of scope, because I want the claim to be narrow:

**I didn't measure exception cost.** The "exceptions are too slow for kernels" argument is real. Our test runs, so we now have a platform on which to measure throw latency in cycles, but I haven't done that yet. Adding timer reads around the throw/catch boundary is the next task and would put real numbers on that tradeoff.

**I didn't measure binary size.** The sizes of `.eh_frame`, `.eh_frame_hdr`, `.gcc_except_table`, and the libsupc++/libgcc_eh text pulled into the kernel are the other half of the cost argument. They're easy to get with `size` and `objdump`. I just haven't done it.

**I didn't enable caches.** The MMU I set up uses Normal Non-Cacheable memory. Enabling D-cache and I-cache is a straightforward next step, and a real kernel would do it, but it wasn't needed for this experiment.

**I didn't install an exception vector table.** I said above that I should have, and that debugging would have been much faster if I had. For the completed M3 result, I just didn't need one — the kernel no longer faults. But any future work here should install one first.

**I didn't test throwing from an interrupt handler.** Exceptions interacting with interrupt context is one of the real, narrow arguments against C++ exceptions in kernels. I didn't address it.

**I didn't integrate into a real kernel.** This is a toy kernel that does nothing except boot, print, allocate, and throw. Whether any of this holds up inside a real kernel subsystem — with preemption, with spinlocks, with RCU — is a separate and much harder question. The point of this experiment is narrower: it establishes that the *runtime machinery* can exist at all in kernel mode. Getting there was mostly about finding the bring-up conditions and meeting them.

## What the Experiment Did Establish

> On aarch64, with GCC 14 or later, the unmodified libsupc++ + libgcc_eh runtime can be made to support **both C++ exceptions and RTTI** in a freestanding kernel — linked statically, under explicit CPU bring-up conditions, with about 250 lines of libc stubs and one symbol override. No runtime patches. No custom unwinder. No forked libgcc. Enabling RTTI on top of a working exception setup is free — it requires removing a single Makefile flag.

That is the claim, narrowed. The phrase I want to avoid is "works out of the box" — it doesn't, and anyone who tries will quickly hit at least a few of the walls above. The right framing is: **the runtime itself does not need to be modified**; what needs attention is the environment *around* it. Specifically, these preconditions have to be met:

1. **FP/SIMD enabled** (`CPACR_EL1.FPEN = 0b11`), before any code — including `.init_array` — that could trigger an inlined NEON memset.
2. **MMU on, with a Normal memory attribute for kernel RAM.** An identity-mapped L1 table is fine; caches are optional. This is the precondition that lets libgcc's inlined NEON stores to unaligned stack addresses succeed.
3. **A heap early enough for `.init_array`.** libsupc++'s `eh_alloc` static constructor calls `malloc` before your `kernel_main` runs.
4. **A terminate handler installed via `std::set_terminate`** that does not reach into libsupc++'s demangling / stderr path.
5. **`__cxa_get_globals` overridden** to return a single file-scope global (or proper TLS set up). The stock libsupc++ implementation is `static __thread`, which requires `TPIDR_EL0`.
6. **`.eh_frame` terminated with a `QUAD(0)` sentinel** in the linker script, and **`__register_frame(&__eh_frame_start)` called once** before any throw.

Each of these is a two-to-ten-line change in boot or runtime code. None of them is architectural work, and none of them requires editing libsupc++ or libgcc_eh. The distinction matters: **the hard cases are all on the kernel side**, where they're your problem anyway if you're bringing up any aarch64 kernel. The runtime side is just accounting.

What convinced me this framing is accurate: the things I expected to be hard — the DWARF unwinder, the FDE lookup, the personality routine, the Itanium two-phase search — worked on the first run once the CPU state matched their assumptions. I had to write no code to make any of that work. I just had to stop breaking it.

What this experiment adds, for me, is one concrete modern data point. The bring-up conditions above are rarely written down in one place, which makes the runtime look more alien to kernel code than it really is. The closest active project in this space is Khalil Estell's [libhal-exceptions](https://github.com/libhal/libhal-exceptions) for Cortex-M, which takes a different tradeoff — it replaces parts of the runtime to shrink binary size and to remove the terminate-handler demangling path. This article points at a different corner of the space: on a platform where binary size is not the main constraint, the replacement work may be unnecessary. The stock runtime runs if you meet its preconditions. Knowing what those preconditions are is most of the work.

---

## Methodology

The kernel was built inside a Docker container running Ubuntu 24.04 with `g++-14` installed from apt. Nothing about the approach requires a GCC trunk — the ATOMIC_FDE_FAST_PATH frame registration and the libsupc++ TLS-based `__cxa_get_globals` that drive Walls 5 and 7 have been stable since GCC 14. Compile flags:

    -std=c++17
    -ffreestanding -nostdlib
    -fasynchronous-unwind-tables
    -Wall -Wextra -O2 -mcpu=cortex-a72

Nothing past C++17 is used in the kernel itself. Link:

    -nostdlib -T linker.ld
    -L<path-to-libsupc++.a>
    -L<path-to-libgcc_eh.a>
    kernel.o cxxrt.o boot.o
    -lsupc++ -lgcc_eh -lgcc

The link order matters: `cxxrt.o` contains the `__cxa_get_globals` override, and it has to appear on the link line *before* `-lsupc++` so that the symbol is resolved from our object before libsupc++'s `eh_globals.o` has a chance to be pulled from the archive. `--whole-archive` is not used, and should not be used — see Wall 2.

Run under QEMU:

    qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M -nographic -kernel build/kernel.elf

Host: MacBook Air M4, 16 GB RAM, macOS 26.3.1 (Darwin 25.3.0).

Debugging technique during the investigation: every stub in `cxxrt.cpp` printed a short tag on entry/exit (`<m:size>`, `<ms:addr,size>`, `<ceg>`, etc.), and functions called through `--wrap`-ed entry points printed their own (`[wrap_throw:entry]`, `[wrap_ure:entry]`, `[wrap_find_fde pc=...]`). This turned the UART output into a trace of the runtime's internal calls, which is what made the seven walls visible instead of one undifferentiated hang. The final kernel has all of this instrumentation removed — the article's trace excerpt is preserved from the debug runs.

---

## What's Next

The next milestones on cppkernel are:

- **M5 (static reflection):** GCC with C++26's `^^T` reflection operator. This is the direction of the larger project; M0 through M4 are infrastructure for M5.
- **M6 (a tiny subsystem):** Use all of M1–M5 to build one small realistic piece of kernel — maybe a typed intrusive list replacing the macro-based `list_head` pattern, with reflection-driven iteration.

The immediate follow-up article I'd like to write is a cost measurement — actual cycle counts for `throw → catch` on the nested test, compared against an equivalent `std::expected<T, E>` implementation on the same paths. That would put numbers on the "cost" question, which this article deliberately stays out of.

If this overlaps with work you're already doing and you'd like to compare notes, feel free to reach out: toneverdo@gmail.com.

The full source of the kernel — about 900 lines including boot, runtime glue, and tests — is in this repository alongside the article.

---

## Appendix: Two Extra Walls from Distro-Built libsupc++

The seven walls above are all about CPU state and C++ runtime assumptions — they are the interesting story. But I want to record two additional walls that only show up when you link against a distribution-packaged libsupc++, because anyone reproducing this with `apt install g++-14` on Ubuntu will hit them and be confused for the same reason I was.

Both walls have the same shape: Ubuntu (and similar distros) compile their bundled libsupc++ with **`-fstack-protector-strong`** and **`-D_FORTIFY_SOURCE=2`** enabled. These produce `libsupc++.a(cp-demangle.o)` with link-time references to symbols that are normally provided by glibc's runtime — symbols that do not exist in a freestanding link.

**Wall 8: `__stack_chk_guard` and `__stack_chk_fail` undefined.** Stack protector emits a check prologue on every non-trivial function: read the canary from `__stack_chk_guard`, save it on the stack, verify it on exit, and call `__stack_chk_fail` if it changed. Freestanding builds don't provide these.

**Wall 9: `__sprintf_chk` and friends undefined.** `-D_FORTIFY_SOURCE=2` activates glibc header macros that rewrite `sprintf(buf, ...)` into `__sprintf_chk(buf, flag, buflen, ...)` for bounds checking. Similarly for `snprintf`, `vsnprintf`, and the `mem*` family. Freestanding builds don't provide any of these either.

The interesting thing about both walls is that **`cp-demangle.o` is never executed in the running kernel.** It's pulled in by the `__verbose_terminate_handler` path, which we short-circuit with our own terminate handler via `std::set_terminate`. The code is dead. But the symbols are referenced at link time regardless — they have to resolve or `ld` refuses to produce an ELF. So the fix is purely cosmetic: provide stub definitions that would panic if called, with the knowledge that they are never called.

All nine stubs together are under 30 lines of code. They go into the same `cxxrt.cpp` as the M3 libc stubs.

If you build your own GCC from source with the defaults used by `./configure && make` inside the GCC source tree, you will not hit Walls 8 or 9, because that build uses a simpler default set of flags for libstdc++-v3. If you use a distro-packaged GCC (which any normal reproduction would), you will hit both. It's not a fact about libsupc++ — it's a fact about how someone else compiled it.

I mention this because it took me a few iterations to realize that the count of walls I hit depended on where my toolchain came from. On my internal dev environment (a Dockerfile that builds GCC 16 trunk from source), the count is seven. On the public Dockerfile (Ubuntu 24.04 + stock `g++-14`), the count is nine. The nine-wall version is the one you'll experience if you reproduce this. I kept the article structured around the seven conceptually interesting walls because walls 8 and 9 are not about C++ or kernels — they're about someone's choice of compilation flags — but they're just as real to anyone running `make` on this repository.
