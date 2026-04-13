# cppkernel

Toy kernels written in modern C++, with full exceptions and RTTI
enabled, running bare-metal on real CPU architectures. Built
against unmodified libsupc++ and libgcc_eh from stock GCC
installations. A series, one directory per architecture.

The motivating question is simple: every embedded C++ guide leads
with `-fno-exceptions` and `-fno-rtti`. On modern toolchains, what
happens if you just... don't disable them?

In practice, the answer is not "the build fails." There are a
small number of CPU-state bring-up conditions and link-order
details the C++ runtime quietly expects, and if you satisfy them,
the whole thing — DWARF stack unwinding, Itanium ABI two-phase
search, typeinfo hierarchy walks, `dynamic_cast` — runs without
patching libgcc, writing a custom unwinder, or forking libsupc++.
This repository is a worked example.

## Articles

| Article | Topic | Architecture |
|---|---|---|
| [Running C++ Exceptions and RTTI in a Bare-Metal aarch64 Kernel](article-exceptions-rtti-aarch64.md) | The seven walls to getting exceptions working | aarch64 |
| [The Real Cost of C++ Exceptions in a Freestanding Kernel](article-freestanding-landscape-aarch64.md) | GCC's freestanding configure, the 11-function surface, C++26 toolbox | aarch64 |

Each article is self-contained. The second builds on the first
but can be read independently. The code for each architecture
lives in a sibling subdirectory of this README.

## Code

| Directory | What it is |
|---|---|
| [`aarch64/`](aarch64/) | aarch64 kernel source, Dockerfile, per-arch README |

Each architecture directory contains its own `src/`, `Makefile`,
`Dockerfile`, and instructions. Building and running one does not
require the others.

## Scope

This is a research-grade experiment, not a production kernel. What
each architecture directory establishes is a narrow, testable
claim: **the stock GCC C++ runtime works in a freestanding kernel
for this architecture, under a documented set of CPU bring-up
conditions.** It does not claim anything about:

- Exception performance in kernel code (not measured).
- Binary size tradeoffs (not catalogued).
- Interrupt-context exception safety (not tested).
- Integration with a real kernel's concurrency primitives
  (this kernel has no concurrency).

Those are the standard reasons cited against C++ exceptions in
kernels, and every one of them remains open. What this project
does provide is a concrete counterexample to the fourth reason —
"the runtime is too entangled with hosted environments to
freestanding-link at all." The claim here is narrower: on these
architectures, under documented bring-up conditions, the stock
runtime can be linked and run as-is.

If you're doing similar work, or have measured any of the open
questions on a real kernel, feel free to reach out. Contact is in
each article.

## License

MIT. See [`LICENSE`](LICENSE).
