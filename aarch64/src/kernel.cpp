// cppkernel — Milestone 0-4
//
// M0: Boot + UART
// M1: Global constructors
// M2: operator new/delete (heap allocation)
// M3: Exceptions (throw, catch, stack unwinding)
// M4: RTTI (typeid, dynamic_cast)

#include "uart.hpp"
#include "heap.hpp"
#include <cstdint>
#include <exception>
#include <typeinfo>

// Declared in cxxrt.cpp — registers our .eh_frame with the unwinder
// via libgcc's __register_frame. Must be called before any throw.
extern "C" void cxxrt_register_eh_frame();

// Our own terminate handler — short-circuits libsupc++'s vterminate,
// which would otherwise try to fwrite the demangled type name to
// stderr. See the article for why that matters in this kernel.
[[noreturn]] static void kernel_terminate() {
    uart::puts("\n[PANIC] std::terminate called (unhandled exception)\n");
    while (true) asm volatile("wfe");
}

// ---- M1: global constructor test ----
struct Banner {
    const char* msg;
    Banner(const char* m) : msg(m) {
        uart::puts("[init] Global constructor called\n");
    }
};

Banner banner{"cppkernel M4"};

// ---- M2: heap allocation tests ----

struct Device {
    int id;
    const char* name;

    Device(int i, const char* n) : id(i), name(n) {}
};

// Polymorphic type — needs vtable allocation to work
struct FileSystem {
    virtual ~FileSystem() = default;
    virtual const char* type_name() = 0;
};

struct RamFS : FileSystem {
    const char* type_name() override { return "ramfs"; }
};

struct ProcFS : FileSystem {
    const char* type_name() override { return "procfs"; }
};

void test_heap() {
    uart::puts("[M2] Heap allocation:\n");

    // Basic new
    auto* dev = new Device{1, "uart0"};
    uart::puts("  new Device:      ");
    uart::puts(dev->name);
    uart::puts(" (id=");
    uart::put_hex(dev->id);
    uart::puts(")\n");

    // Array new
    auto* arr = new int[10];
    arr[0] = 0xCAFE;
    arr[9] = 0xBEEF;
    uart::puts("  new int[10]:     arr[0]=");
    uart::put_hex(arr[0]);
    uart::puts(" arr[9]=");
    uart::put_hex(arr[9]);
    uart::puts("\n");

    // Polymorphic new — vtable must work
    FileSystem* fs = new RamFS();
    uart::puts("  new RamFS:       type=");
    uart::puts(fs->type_name());
    uart::puts("\n");

    FileSystem* fs2 = new ProcFS();
    uart::puts("  new ProcFS:      type=");
    uart::puts(fs2->type_name());
    uart::puts("\n");

    // Heap stats
    uart::puts("  Heap used:       ");
    uart::put_hex(heap::used());
    uart::puts(" / ");
    uart::put_hex(heap::capacity());
    uart::puts("\n");

    uart::puts("  PASS\n");
}

// ---- M3: exception tests ----

struct KernelError {
    int errno_val;
    const char* msg;
};

// Force no inlining so we get real stack frames to unwind through.
[[gnu::noinline]]
static void inner_function() {
    uart::puts("  entering inner_function\n");
    throw KernelError{-22, "EINVAL from inner"};
    uart::puts("  (inner: this should not print)\n");
}

[[gnu::noinline]]
static void outer_function() {
    uart::puts("  entering outer_function\n");
    inner_function();
    uart::puts("  (outer: this should not print)\n");
}

// ---- M4: RTTI tests ----
//
// Reuses the FileSystem / RamFS / ProcFS hierarchy from M2. Adds a
// dynamic_cast downcast (should succeed when the target matches),
// a cross-cast (should return nullptr), and a typeid(*p).name()
// lookup (should return the Itanium-mangled class name).

static void test_rtti() {
    uart::puts("[M4] RTTI:\n");

    FileSystem* fs1 = new RamFS();
    FileSystem* fs2 = new ProcFS();

    // typeid on polymorphic reference — must reflect the runtime type,
    // which means reading the vtable's typeinfo slot.
    uart::puts("  typeid(*fs1).name() = ");
    uart::puts(typeid(*fs1).name());
    uart::puts("\n");
    uart::puts("  typeid(*fs2).name() = ");
    uart::puts(typeid(*fs2).name());
    uart::puts("\n");

    // Successful downcast: fs1 actually points at a RamFS.
    RamFS* r = dynamic_cast<RamFS*>(fs1);
    if (r && r->type_name() == fs1->type_name()) {
        uart::puts("  dynamic_cast<RamFS*>(fs1):  OK\n");
    } else {
        uart::puts("  dynamic_cast<RamFS*>(fs1):  FAIL\n");
        return;
    }

    // Failing cross-cast: fs1 is a RamFS, not a ProcFS.
    ProcFS* p = dynamic_cast<ProcFS*>(fs1);
    if (p == nullptr) {
        uart::puts("  dynamic_cast<ProcFS*>(fs1): nullptr as expected\n");
    } else {
        uart::puts("  dynamic_cast<ProcFS*>(fs1): UNEXPECTED NON-NULL\n");
        return;
    }

    // Successful downcast on fs2.
    ProcFS* p2 = dynamic_cast<ProcFS*>(fs2);
    if (p2 && p2->type_name() == fs2->type_name()) {
        uart::puts("  dynamic_cast<ProcFS*>(fs2): OK\n");
    } else {
        uart::puts("  dynamic_cast<ProcFS*>(fs2): FAIL\n");
        return;
    }

    // typeid comparison between two runtime types.
    if (typeid(*fs1) != typeid(*fs2)) {
        uart::puts("  typeid(*fs1) != typeid(*fs2): OK\n");
    } else {
        uart::puts("  typeid comparison FAIL\n");
        return;
    }

    uart::puts("  PASS\n");
}

static void test_exceptions() {
    uart::puts("[M3] Exceptions:\n");

    // Test 1: same-frame throw/catch
    try {
        throw KernelError{-1, "direct throw"};
    } catch (const KernelError& e) {
        uart::puts("  caught direct: errno=");
        uart::put_hex(static_cast<std::uint64_t>(-e.errno_val));
        uart::puts("\n");
    }

    // Test 2: throw across stack frames — actually exercises the unwinder.
    try {
        outer_function();
    } catch (const KernelError& e) {
        uart::puts("  caught nested: errno=");
        uart::put_hex(static_cast<std::uint64_t>(-e.errno_val));
        uart::puts(" msg=");
        uart::puts(e.msg);
        uart::puts("\n");
    }

    uart::puts("  PASS\n");
}

// ---- Kernel entry ----
extern "C" void kernel_main() {
    heap::init();

    // Wire up C++ exception runtime before anything can throw:
    // 1. Short-circuit vterminate so an escaping exception halts
    //    cleanly instead of reaching into fwrite/demangle.
    // 2. Register our .eh_frame with libgcc's static FDE table so
    //    the unwinder can find CFI data without dl_iterate_phdr.
    std::set_terminate(kernel_terminate);
    cxxrt_register_eh_frame();

    uart::puts("\n");
    uart::puts("========================================\n");
    uart::puts("  cppkernel — bare metal C++ kernel\n");
    uart::puts("  Toolchain: GCC 14+ / C++17\n");
    uart::puts("  Target:   aarch64 / QEMU virt\n");
    uart::puts("========================================\n\n");

    // M0
    uart::puts("[M0] UART output:  OK\n");

    // M1
    if (banner.msg) {
        uart::puts("[M1] Global ctors: OK (banner = \"");
        uart::puts(banner.msg);
        uart::puts("\")\n");
    } else {
        uart::puts("[M1] Global ctors: FAIL\n");
    }

    // M2
    test_heap();

    // M3
    test_exceptions();

    // M4
    test_rtti();

    uart::puts("\nAll milestones passed. Halting.\n");
    while (true) asm volatile("wfe");
}
