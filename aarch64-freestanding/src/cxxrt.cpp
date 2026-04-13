// cxxrt.cpp — The irreducible C++ runtime surface for a
// freestanding aarch64 kernel with exceptions and RTTI.
//
// GCC configured with:
//   --disable-hosted-libstdcxx
//   --disable-tls
//   --enable-threads=single
//   --enable-libstdcxx-static-eh-pool --with-libstdcxx-eh-pool-obj-count=0
//   --disable-libstdcxx-verbose
//
// This file contains ONLY the symbols that the linker actually
// demands. Nothing more. Every symbol has a comment explaining
// who needs it and why it can't be eliminated.
//
// Total: ~60 lines. Down from ~300 with a hosted libsupc++.

#include "heap.hpp"
#include "uart.hpp"
#include <cstddef>
#include <cstdint>

// ==========================================================================
// C++ ABI basics (needed by kernel code, not by the exception runtime)
// ==========================================================================

void* operator new(std::size_t size) {
    void* p = heap::alloc(size);
    if (!p) { uart::puts("[PANIC] OOM\n"); while (true) asm volatile("wfe"); }
    return p;
}
void* operator new[](std::size_t size)    { return operator new(size); }
void operator delete(void*) noexcept      {}
void operator delete[](void*) noexcept    {}
void operator delete(void*, std::size_t) noexcept   {}
void operator delete[](void*, std::size_t) noexcept {}

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
void* __dso_handle = nullptr;
extern "C" void __cxa_pure_virtual() {
    uart::puts("[PANIC] pure virtual\n");
    while (true) asm volatile("wfe");
}

// ==========================================================================
// The 11 symbols the C++ exception + RTTI runtime actually needs
// ==========================================================================

extern "C" {

// --- Allocator (libsupc++ __cxa_allocate/free_exception) -----------------
void* malloc(std::size_t size) { return heap::alloc(size); }
void  free(void*)              {}  // bump allocator
void* calloc(std::size_t n, std::size_t size) {
    std::size_t total = n * size;
    void* p = heap::alloc(total);
    if (p) __builtin_memset(p, 0, total);
    return p;
}

// --- Panic (libsupc++ std::terminate default path) -----------------------
[[noreturn]] void abort() {
    uart::puts("[PANIC] abort\n");
    while (true) asm volatile("wfe");
}

// --- Memory ops (libgcc_eh DWARF CFI interpreter) ------------------------
void* memcpy(void* d, const void* s, std::size_t n) {
    auto* dst = static_cast<std::uint8_t*>(d);
    auto* src = static_cast<const std::uint8_t*>(s);
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    return d;
}
void* memmove(void* d, const void* s, std::size_t n) {
    auto* dst = static_cast<std::uint8_t*>(d);
    auto* src = static_cast<const std::uint8_t*>(s);
    if (dst < src) for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    else           for (std::size_t i = n; i-- > 0;)   dst[i] = src[i];
    return d;
}
void* memset(void* d, int c, std::size_t n) {
    auto* dst = static_cast<std::uint8_t*>(d);
    for (std::size_t i = 0; i < n; ++i) dst[i] = static_cast<std::uint8_t>(c);
    return d;
}

// --- String ops (libsupc++ type_info + libgcc_eh CIE augmentation) -------
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
std::size_t strlen(const char* s) {
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

// --- Platform stubs (aarch64-specific, each 1 line) ----------------------
// libgcc reads AT_HWCAP from ELF aux vector for LSE/SME detection.
// No aux vector in a kernel. Return 0 → baseline instruction set.
unsigned long __getauxval(unsigned long) { return 0; }

// libgcc_eh's DIP uses _dl_find_object to locate .eh_frame via the
// dynamic loader. No loader in a kernel. Return -1 → fall back to
// the static __register_frame registry.
int _dl_find_object(void*, void*) { return -1; }

// --- FDE registration (our one setup call) -------------------------------
extern void __register_frame(void*);
extern char __eh_frame_start[];
void cxxrt_register_eh_frame() {
    __register_frame(__eh_frame_start);
}

} // extern "C"
