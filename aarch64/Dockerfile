# cppkernel aarch64 — reproducible build environment
#
# This Dockerfile sets up everything needed to build and run the
# aarch64 kernel. It uses stock GCC 14 from Ubuntu 24.04's package
# repositories. Build time is ~3-5 minutes (dominated by `apt-get
# update`), compared to 60+ minutes if you were building GCC from
# source. Nothing in cppkernel through M4 requires any compiler
# newer than GCC 14.
#
# Usage (from the aarch64/ directory):
#
#   docker build -t cppkernel-aarch64 .
#   docker run --rm -it -v "$(pwd)/src:/src" cppkernel-aarch64 \
#       bash -c "cd /src && make CXX=g++-14"
#
# Then on the host, with qemu-system-aarch64 installed:
#
#   cd src && qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M \
#       -nographic -kernel build/kernel.elf
#
# (Quit QEMU with Ctrl-A, X.)

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# g++-14: the compiler.
# qemu-system-arm: for the `make run` target, if you want to run
#                  the kernel inside the container instead of on
#                  the host.
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-14 \
        make \
        qemu-system-arm \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Make g++-14 the default g++ for convenience.
RUN update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

WORKDIR /src

CMD ["/bin/bash"]
