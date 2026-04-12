// C++ runtime support for bare metal
// Provides: operator new/delete, __cxa_atexit, __cxa_pure_virtual,
//           and the minimal libc / pthread surface libsupc++ and
//           libgcc_eh reach for.
//
// See ../../article-exceptions-rtti-aarch64.md for why each of these
// is here, which of them are load-bearing, and which are only
// resolving linker references.

#include "heap.hpp"
#include "uart.hpp"
#include <cstddef>
#include <cstdint>

// ==========================================================================
// operator new / delete
// ==========================================================================

void* operator new(std::size_t size) {
    void* p = heap::alloc(size);
    if (!p) {
        uart::puts("[PANIC] operator new: out of memory\n");
        while (true) asm volatile("wfe");
    }
    return p;
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete(void*) noexcept {
    // Bump allocator: no-op. Memory is never freed.
}

void operator delete[](void* p) noexcept {
    operator delete(p);
}

void operator delete(void*, std::size_t) noexcept {}
void operator delete[](void*, std::size_t) noexcept {}

// ==========================================================================
// C++ ABI support
// ==========================================================================

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) {
    // Global destructors: in a kernel we never "exit", so drop them.
    return 0;
}

void* __dso_handle = nullptr;

extern "C" void __cxa_pure_virtual() {
    uart::puts("[PANIC] pure virtual function called\n");
    while (true) asm volatile("wfe");
}

// ==========================================================================
// Minimal libc surface for libsupc++ / libgcc_eh
// ==========================================================================
//
// These are symbols libsupc++ and libgcc_eh want resolved at link
// time. In a single-threaded kernel most have no meaningful
// semantics, so most are no-ops. The article explains which ones
// are load-bearing and which are only there to satisfy the linker.

extern "C" {

// ---- allocation ---------------------------------------------------------
//
// libsupc++ eh_alloc calls malloc to build its exception pool during
// .init_array (BEFORE kernel_main). heap::alloc lazy-inits on first
// use, so this works despite the ordering.

void* malloc(std::size_t size)            { return heap::alloc(size); }
void  free(void*)                         { /* bump allocator */ }

void* calloc(std::size_t n, std::size_t size) {
    std::size_t total = n * size;
    void* p = heap::alloc(total);
    if (p) __builtin_memset(p, 0, total);
    return p;
}

void* realloc(void*, std::size_t size) {
    // Bump allocator can't actually resize; allocate fresh.
    return heap::alloc(size);
}

// ---- abort --------------------------------------------------------------
//
// libsupc++ and libgcc call abort on fatal conditions. We halt.

[[noreturn]] void abort() {
    uart::puts("[PANIC] abort() called from C++ runtime\n");
    while (true) asm volatile("wfe");
}

// ---- mem / str ops ------------------------------------------------------
//
// GCC usually inlines these, but libsupc++/libgcc can still emit
// out-of-line calls. strcmp in particular is on the live exception
// catch path — type_info::operator== compares mangled type names
// with strcmp, even with -fno-rtti.

void* memcpy(void* dst, const void* src, std::size_t n) {
    auto* d = static_cast<std::uint8_t*>(dst);
    auto* s = static_cast<const std::uint8_t*>(src);
    for (std::size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

void* memset(void* dst, int c, std::size_t n) {
    auto* d = static_cast<std::uint8_t*>(dst);
    for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<std::uint8_t>(c);
    return dst;
}

void* memmove(void* dst, const void* src, std::size_t n) {
    auto* d = static_cast<std::uint8_t*>(dst);
    auto* s = static_cast<const std::uint8_t*>(src);
    if (d < s) {
        for (std::size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (std::size_t i = n; i-- > 0; ) d[i] = s[i];
    }
    return dst;
}

int memcmp(const void* a, const void* b, std::size_t n) {
    auto* x = static_cast<const std::uint8_t*>(a);
    auto* y = static_cast<const std::uint8_t*>(b);
    for (std::size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

std::size_t strlen(const char* s) {
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == c) return const_cast<char*>(s);
        ++s;
    }
    return (c == 0) ? const_cast<char*>(s) : nullptr;
}

int strncmp(const char* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char x = a[i], y = b[i];
        if (x != y) return x - y;
        if (x == 0) return 0;
    }
    return 0;
}

// ---- environment probes -------------------------------------------------
//
// libsupc++'s eh_alloc reads GLIBCXX_EXCEPTION_POOL_SIZE. libgcc's
// LSE/SME initializers read AT_HWCAP from the ELF aux vector. None
// of these exist in a kernel; returning nothing is the right answer.

char* secure_getenv(const char*)         { return nullptr; }
char* getenv(const char*)                { return nullptr; }
unsigned long __getauxval(unsigned long) { return 0; }
unsigned long getauxval(unsigned long)   { return 0; }

unsigned long __isoc23_strtoul(const char* s, char** end, int /*base*/) {
    // eh_alloc only calls this to parse the pool-size env var, which
    // always returns null — we should never reach here with real input.
    if (end) *end = const_cast<char*>(s);
    return 0;
}

// ---- pthread (single-threaded kernel) -----------------------------------
//
// libsupc++'s eh_alloc mutex-protects its pool. libgcc's FDE btree
// uses a version-lock + mutex for concurrent registration. libgcc's
// emutls uses pthread_key_*. We are single-threaded, so all of this
// is a no-op.

int pthread_mutex_lock(void*)              { return 0; }
int pthread_mutex_unlock(void*)            { return 0; }
int pthread_mutex_trylock(void*)           { return 0; }
int pthread_mutex_init(void*, const void*) { return 0; }
int pthread_mutex_destroy(void*)           { return 0; }

int pthread_key_create(unsigned*, void (*)(void*)) { return 0; }
int pthread_key_delete(unsigned)                    { return 0; }
void* pthread_getspecific(unsigned)                 { return nullptr; }
int pthread_setspecific(unsigned, const void*)      { return 0; }

int pthread_once(int*, void (*init)(void)) {
    static int done = 0;
    if (!done) { done = 1; init(); }
    return 0;
}

int pthread_cond_wait(void*, void*) { return 0; }
int pthread_cond_broadcast(void*)   { return 0; }
int pthread_cond_signal(void*)      { return 0; }

// ---- loader -------------------------------------------------------------
//
// libgcc's unwind-dw2-fde-dip.c uses this to walk loaded ELF
// objects at runtime. Returning -1 causes libgcc to fall back to
// its statically-registered FDE table, which we populate via
// __register_frame from kernel_main.

int _dl_find_object(void*, void*) { return -1; }

// ---- vterminate dead weight ---------------------------------------------
//
// Unreachable if std::set_terminate has installed our own handler.
// Symbols still need to resolve because libsupc++'s default terminate
// handler references them. If any of these actually fires, that's
// itself a finding.

void* stderr = nullptr;

std::size_t fwrite(const void*, std::size_t, std::size_t, void*) {
    uart::puts("[PANIC] fwrite stub invoked — terminate handler escaped\n");
    while (true) asm volatile("wfe");
}

int fputs(const char*, void*) {
    uart::puts("[PANIC] fputs stub invoked\n");
    while (true) asm volatile("wfe");
}

int fputc(int, void*) {
    uart::puts("[PANIC] fputc stub invoked\n");
    while (true) asm volatile("wfe");
}

int sprintf(char*, const char*, ...) {
    uart::puts("[PANIC] sprintf stub invoked\n");
    while (true) asm volatile("wfe");
}

// ---- stack protector stubs ---------------------------------------------
//
// Distro-packaged libsupc++ (Ubuntu 24.04's g++-14 at least) is built
// with -fstack-protector-strong, so cp-demangle.o has link-time
// references to __stack_chk_guard and __stack_chk_fail. cp-demangle.o
// is never actually executed in our kernel — we override
// std::terminate, so the verbose_terminate_handler → demangler chain
// is dead — but the symbols still need to resolve at link time.
//
// GCC trunk built from source without -fstack-protector does not
// exhibit this, so internal dev builds missed it; it only shows up
// when linking against distro libsupc++.

unsigned long __stack_chk_guard = 0xdeadbeefcafebabeULL;

[[noreturn]] void __stack_chk_fail() {
    uart::puts("[PANIC] __stack_chk_fail (unreachable path executed)\n");
    while (true) asm volatile("wfe");
}

// ---- _FORTIFY_SOURCE stubs ---------------------------------------------
//
// Same story as the stack protector: distro libsupc++ is built with
// -D_FORTIFY_SOURCE=2, so glibc's fortify macros rewrite calls to
// sprintf/snprintf/memcpy/etc. into their __*_chk variants. These
// are referenced from libsupc++'s cp-demangle.o, which, again, is
// never actually executed because we override std::terminate. All
// of these are panic stubs.

int __sprintf_chk(char*, int, unsigned long, const char*, ...) {
    uart::puts("[PANIC] __sprintf_chk stub invoked\n");
    while (true) asm volatile("wfe");
}

int __snprintf_chk(char*, unsigned long, int, unsigned long, const char*, ...) {
    uart::puts("[PANIC] __snprintf_chk stub invoked\n");
    while (true) asm volatile("wfe");
}

int __vsnprintf_chk(char*, unsigned long, int, unsigned long, const char*, void*) {
    uart::puts("[PANIC] __vsnprintf_chk stub invoked\n");
    while (true) asm volatile("wfe");
}

void* __memcpy_chk(void* dst, const void* src, std::size_t n, std::size_t) {
    return memcpy(dst, src, n);
}

void* __memmove_chk(void* dst, const void* src, std::size_t n, std::size_t) {
    return memmove(dst, src, n);
}

void* __memset_chk(void* dst, int c, std::size_t n, std::size_t) {
    return memset(dst, c, n);
}

// ==========================================================================
// __cxa_get_globals / __cxa_get_globals_fast — override libsupc++
// ==========================================================================
//
// libsupc++'s eh_globals.cc implements this as
//   `static __thread __cxa_eh_globals global;`
// which compiles to a TPIDR_EL0 read on aarch64. Our kernel does
// not set up TPIDR_EL0, so we define these ourselves in a TU that
// the linker sees BEFORE libsupc++.a — eh_globals.o then never gets
// pulled from the archive.
//
// Layout must match libsupc++/unwind-cxx.h's __cxa_eh_globals:
//   struct { void* caughtExceptions; unsigned int uncaughtExceptions; };
// aarch64 is not __ARM_EABI_UNWINDER__, so no propagatingExceptions.

struct cxa_eh_globals_layout {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
};

static cxa_eh_globals_layout kernel_eh_globals = {nullptr, 0};

void* __cxa_get_globals()      noexcept { return &kernel_eh_globals; }
void* __cxa_get_globals_fast() noexcept { return &kernel_eh_globals; }

// ==========================================================================
// FDE registration
// ==========================================================================
//
// Called once from kernel_main before any throw, to hand libgcc's
// unwinder our .eh_frame. The sentinel QUAD(0) in linker.ld (crtend
// normally provides it in hosted builds) tells __register_frame
// where the FDE list ends.

extern void __register_frame(void*);
extern char __eh_frame_start[];

void cxxrt_register_eh_frame() {
    __register_frame(__eh_frame_start);
}

} // extern "C"
