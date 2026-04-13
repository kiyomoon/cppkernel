#pragma once
// Bump allocator — the simplest possible kernel heap.
// No free(), no reuse. Good enough to prove C++ runtime works.
// A real kernel would use a slab or buddy allocator.

#include <cstdint>
#include <cstddef>

namespace heap {

// Defined in linker.ld
extern "C" {
    extern char __heap_start[];
    extern char __heap_end[];
}

namespace detail {
    inline char* next = nullptr;
}

// Idempotent init — DO NOT reset detail::next unconditionally.
// libsupc++'s eh_alloc has an .init_array static ctor that allocates
// its exception pool before kernel_main ever runs; if this function
// resets next, kernel_main's later allocations will overwrite that
// pool and the next __cxa_allocate_exception will corrupt live memory.
// Lazy init in alloc() below handles the case where nothing has
// allocated yet.
inline void init() {
    if (!detail::next) detail::next = __heap_start;
}

inline void* alloc(std::size_t size, std::size_t align = 16) {
    // Lazy init — libsupc++'s eh_alloc has a .init_array static ctor
    // that calls malloc BEFORE kernel_main runs, so we can't rely on
    // the explicit heap::init() call. Detect the zero-BSS state and
    // fix it up on first use.
    if (!detail::next) detail::next = __heap_start;

    // Align up
    auto addr = reinterpret_cast<std::uintptr_t>(detail::next);
    addr = (addr + align - 1) & ~(align - 1);

    auto* result = reinterpret_cast<void*>(addr);
    detail::next = reinterpret_cast<char*>(addr + size);

    // Out of memory check
    if (detail::next > __heap_end) {
        return nullptr; // M3 will turn this into an exception
    }

    return result;
}

inline std::size_t used() {
    return detail::next - __heap_start;
}

inline std::size_t capacity() {
    return __heap_end - __heap_start;
}

} // namespace heap
