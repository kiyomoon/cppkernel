# ==============================================================
# cppkernel development environment
#
# GCC trunk with freestanding libsupc++ — purpose-built for
# bare-metal C++ kernel development with exceptions and RTTI.
#
# Key configure options for the runtime library:
#   --disable-hosted-libstdcxx        _GLIBCXX_HOSTED=0
#   --enable-libstdcxx-static-eh-pool _GLIBCXX_EH_POOL_STATIC
#   --with-libstdcxx-eh-pool-obj-count=0  USE_POOL=0
#   --disable-libstdcxx-verbose       _GLIBCXX_VERBOSE=0
#
# These produce a libsupc++.a that:
#   - Has NO .init_array entries from eh_alloc
#   - Uses a single __constinit global for __cxa_get_globals
#     (no __thread TLS, no pthread_key)
#   - Uses std::abort as terminate handler (no vterminate, no
#     cp-demangle, no fwrite/fputs/stderr)
#   - Needs only malloc/free/memset from the freestanding environment
#
# Build time: ~60-90 min on Apple Silicon (first time only).
#
#   docker build -f Dockerfile.kernel-dev -t kernel-dev .
#   docker run -it --name kernel-dev -v $(pwd)/work:/work kernel-dev
# ==============================================================

FROM ubuntu:24.04 AS gcc-builder

ENV DEBIAN_FRONTEND=noninteractive

# ---- Build dependencies for GCC ----
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    flex \
    bison \
    libgmp-dev \
    libmpfr-dev \
    libmpc-dev \
    libisl-dev \
    texinfo \
    wget \
    rsync \
    curl \
    ca-certificates \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- Clone GCC trunk (shallow) ----
RUN git clone --depth 1 git://gcc.gnu.org/git/gcc.git /opt/gcc-src

# ---- Build GCC with freestanding libsupc++ ----
#
# --disable-bootstrap: skip 3-stage self-build (~2/3 of build time saved)
# --enable-languages=c,c++: we only need C and C++
# --disable-multilib: no 32-bit support needed
# --disable-hosted-libstdcxx: the main event — freestanding libsupc++
# --enable-libstdcxx-static-eh-pool: static exception pool (no malloc)
# --with-libstdcxx-eh-pool-obj-count=0: eliminate pool entirely (USE_POOL=0)
# --disable-libstdcxx-verbose: abort as default terminate handler
#
# Step 1: Configure
# Note: --disable-hosted-libstdcxx sets _GLIBCXX_HOSTED=0 in the
# shared C++ headers. Other runtime libraries (libitm, libsanitizer,
# libgomp) that share those headers will fail to compile because they
# expect hosted declarations like free(). Since we only need libgcc
# + libsupc++ for kernel work, disable everything else.
RUN mkdir /opt/gcc-build && cd /opt/gcc-build && \
    /opt/gcc-src/configure \
        --prefix=/opt/gcc-16 \
        --enable-languages=c,c++ \
        --disable-multilib \
        --disable-bootstrap \
        --disable-hosted-libstdcxx \
        --disable-tls \
        --enable-threads=single \
        --enable-libstdcxx-static-eh-pool \
        --with-libstdcxx-eh-pool-obj-count=0 \
        --disable-libstdcxx-verbose \
        --disable-libitm \
        --disable-libsanitizer \
        --disable-libgomp \
        --disable-libquadmath \
        --disable-libssp \
        --disable-libvtv \
        --disable-libstdcxx-pch

# Step 2: Build (this is the long part — ~60-90 min)
RUN cd /opt/gcc-build && make -j$(nproc)

# Step 3: Install
RUN cd /opt/gcc-build && make install

# ==============================================================
# Final image: GCC with freestanding runtime + QEMU + dev tools
# ==============================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    bc \
    flex \
    bison \
    libelf-dev \
    libssl-dev \
    wget \
    curl \
    vim \
    less \
    ca-certificates \
    zlib1g-dev \
    libgmp-dev \
    libmpfr-dev \
    libmpc-dev \
    libisl-dev \
    qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*

# ---- Copy GCC from builder stage ----
COPY --from=gcc-builder /opt/gcc-16 /opt/gcc-16
# ---- Keep GCC source for reference / modification ----
COPY --from=gcc-builder /opt/gcc-src /opt/gcc-src

ENV PATH="/opt/gcc-16/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/gcc-16/lib64"

# ---- Verify the freestanding libsupc++ was built correctly ----
# Check that __cxa_get_globals in the installed libsupc++.a does NOT
# reference __thread TLS (TPIDR_EL0), and that eh_alloc.o does NOT
# reference secure_getenv or pthread_mutex_lock.
# Verify the freestanding libsupc++ + libgcc were built correctly.
# With --disable-tls --enable-threads=single --disable-hosted-libstdcxx:
#   - eh_globals.o should NOT reference TPIDR_EL0 or pthread_key
#   - eh_alloc.o should NOT reference secure_getenv or pthread_mutex
#   - libgcc_eh should NOT reference pthread_mutex or pthread_cond
RUN echo "=== Verifying freestanding runtime ===" && \
    cd /tmp && \
    echo "-- libsupc++ eh_alloc.o --" && \
    ar x /opt/gcc-16/lib64/libsupc++.a eh_alloc.o eh_globals.o && \
    nm -u eh_alloc.o | sort -u && \
    echo "-- libsupc++ eh_globals.o --" && \
    nm -u eh_globals.o | sort -u && \
    echo "-- checking libgcc_eh for pthread --" && \
    GCC_LIBDIR=$(dirname $(g++ -print-libgcc-file-name)) && \
    nm -u $GCC_LIBDIR/libgcc_eh.a 2>/dev/null | sort -u | grep -E "pthread|emutls|tpidr" || echo "  (none found)" && \
    echo "=== Absence checks ===" && \
    ! nm -u eh_alloc.o | grep -q "secure_getenv" && echo "  secure_getenv: absent (good)" && \
    ! nm -u eh_alloc.o | grep -q "pthread" && echo "  pthread in eh_alloc: absent (good)" && \
    ! nm -u eh_globals.o | grep -q "pthread" && echo "  pthread in eh_globals: absent (good)" && \
    ! nm -u eh_globals.o | grep -q "tpidr" && echo "  tpidr in eh_globals: absent (good)" && \
    echo "=== Freestanding verification passed ===" && \
    rm -f eh_alloc.o eh_globals.o

WORKDIR /work

CMD ["/bin/bash"]
