# cppkernel — aarch64 (freestanding libsupc++)

The same toy kernel as [`../aarch64/`](../aarch64/), but built
against a **freestanding-configured GCC** — the libsupc++ that
ships from this build needs only 11 external symbols from the
kernel, compared to ~30 with a stock hosted build.

For the full story, see the article one directory up:
[`../article-freestanding-landscape-aarch64.md`](../article-freestanding-landscape-aarch64.md).

## Key difference from `aarch64/`

| | `aarch64/` (hosted libsupc++) | `aarch64-freestanding/` (this) |
|---|---|---|
| GCC build | stock `g++-14` from apt | GCC trunk built from source with freestanding flags |
| `cxxrt.cpp` | ~300 lines, 28 stubs | **~60 lines, 11 stubs** |
| pthread stubs | 13 | **0** |
| TLS override | needed | **not needed** |
| Dockerfile build time | ~3 min (apt install) | ~10-15 min (GCC from source) |

## Build

```bash
# Build the Docker image (includes GCC compilation — ~10-15 min first time)
docker build -t cppkernel-freestanding .

# Build the kernel
docker run --rm -v "$(pwd)/src:/src" cppkernel-freestanding \
    bash -c "cd /src && make clean && make"

# Run (on host with QEMU, or inside the container via make run)
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M -nographic \
    -kernel src/build/kernel.elf
```

## The GCC configure recipe

The Dockerfile builds GCC trunk with:

```
--disable-hosted-libstdcxx        # _GLIBCXX_HOSTED=0
--disable-tls                     # no __thread TLS
--enable-threads=single           # no pthread/gthread
--enable-libstdcxx-static-eh-pool # static exception pool
--with-libstdcxx-eh-pool-obj-count=0  # pool compiled out
--disable-libstdcxx-verbose       # abort as terminate handler
```

These produce a libsupc++.a where `eh_globals.o` has zero
undefined symbols and `eh_alloc.o` needs only `malloc`/`free`/
`calloc`/`std::terminate`.

## Expected output

Same as `aarch64/` — all milestones pass:

```
[M0] UART output:  OK
[M1] Global ctors: OK (banner = "cppkernel M4")
[M2] Heap allocation: ... PASS
[M3] Exceptions: ... PASS
[M4] RTTI: ... PASS
All milestones passed. Halting.
```
