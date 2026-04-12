# cppkernel — aarch64

A freestanding aarch64 toy kernel written in C++ with full
exceptions and RTTI enabled, running on QEMU virt. Built against
unmodified libsupc++ and libgcc_eh from a stock GCC 14 install.

For the full story of why this is interesting and what it took to
make it work, see the article one directory up:
[`../article-exceptions-rtti-aarch64.md`](../article-exceptions-rtti-aarch64.md).

## What's in here

| File | What it is |
|---|---|
| `src/boot.S` | aarch64 entry point — EL1 dispatch, FPU enable, MMU bring-up, BSS zero, `.init_array`, jump to C++ |
| `src/kernel.cpp` | Test driver for M0 through M4 (UART, global ctors, heap, exceptions, RTTI) |
| `src/cxxrt.cpp` | Libc stub surface + `__cxa_get_globals` override + `__register_frame` caller |
| `src/heap.hpp` | 512KB bump allocator |
| `src/uart.hpp` | PL011 driver at `0x09000000` |
| `src/linker.ld` | Entry at `0x40080000`, standard section layout + `.eh_frame` sentinel |
| `src/Makefile` | Build rules |
| `Dockerfile` | Reproducible build environment (Ubuntu 24.04 + g++-14) |

## Build

### The Docker path (recommended, reproducible)

```bash
docker build -t cppkernel-aarch64 .
docker run --rm -it -v "$(pwd)/src:/src" cppkernel-aarch64 \
    bash -c "cd /src && make CXX=g++-14"
```

The build takes a few seconds once the image is ready. The image
itself is ~3–5 minutes on first `docker build` (mostly `apt-get
update`).

### The native path

If you have GCC 14+ for aarch64 available directly (native on an
Apple Silicon Mac, a Linux/aarch64 machine, or as a cross-compiler),
you can just build in `src/`:

```bash
cd src
make CXX=g++-14
```

Anything from GCC 14 onward should work. `g++-15`, `g++-16` trunk,
etc. are all fine. `g++-13` and earlier use a different FDE
registration path inside libgcc and have not been tested — you
might need to adjust M3's Wall 4 / Wall 5 approach.

## Run

On the host (not in the container), with QEMU installed:

```bash
cd src
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M -nographic \
    -kernel build/kernel.elf
```

Quit QEMU with `Ctrl-A` then `X`.

You can also run inside the Docker container via `make run`, which
is convenient if you don't have QEMU installed on the host:

```bash
docker run --rm -it -v "$(pwd)/src:/src" cppkernel-aarch64 \
    bash -c "cd /src && make CXX=g++-14 run"
```

## Expected output

```
[init] Global constructor called

========================================
  cppkernel — bare metal C++ kernel
  Toolchain: GCC 14+ / C++17
  Target:   aarch64 / QEMU virt
========================================

[M0] UART output:  OK
[M1] Global ctors: OK (banner = "cppkernel M4")
[M2] Heap allocation:
  new Device:      uart0 (id=0x1)
  new int[10]:     arr[0]=0xcafe arr[9]=0xbeef
  new RamFS:       type=ramfs
  new ProcFS:      type=procfs
  Heap used:       0x12230 / 0x80000
  PASS
[M3] Exceptions:
  caught direct: errno=0x1
  entering outer_function
  entering inner_function
  caught nested: errno=0x16 msg=EINVAL from inner
  PASS
[M4] RTTI:
  typeid(*fs1).name() = 5RamFS
  typeid(*fs2).name() = 6ProcFS
  dynamic_cast<RamFS*>(fs1):  OK
  dynamic_cast<ProcFS*>(fs1): nullptr as expected
  dynamic_cast<ProcFS*>(fs2): OK
  typeid(*fs1) != typeid(*fs2): OK
  PASS

All milestones passed. Halting.
```

After `All milestones passed. Halting.`, the kernel spins in `wfe`
and QEMU keeps running until you exit it with `Ctrl-A X`.
