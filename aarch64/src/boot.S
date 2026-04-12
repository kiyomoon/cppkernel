// cppkernel — aarch64 entry point
//
// Bring-up order (each step matters — see the article for why):
//   1. Drop to EL1 if QEMU started us at EL2
//   2. Set up SP_EL1
//   3. Enable FP/SIMD (CPACR_EL1.FPEN)           ← GCC uses NEON memset
//   4. Zero BSS                                  ← must precede MMU
//                                                  (page table is in .bss)
//   5. Set up identity-mapped MMU                ← Normal memory for RAM
//                                                  so unaligned NEON works
//   6. Walk .init_array (global constructors)
//   7. Jump to kernel_main()

.section .text._start
.globl _start

_start:
    // ------------------------------------------------------------------
    // If booted at EL2, drop to EL1. QEMU -M virt -kernel may start at
    // EL1 or EL2 depending on machine config; handle both.
    // ------------------------------------------------------------------
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    at_el1

    mov     x0, #(1 << 31)          // HCR_EL2.RW = 1: EL1 is aarch64
    msr     hcr_el2, x0

    mov     x0, #0x3c5              // DAIF masked, M = 0b0101 (EL1h)
    msr     spsr_el2, x0

    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    // Set up stack pointer
    ldr     x0, =__stack_top
    mov     sp, x0

    // ------------------------------------------------------------------
    // Enable FP/SIMD. CPACR_EL1.FPEN = 0b00 on reset traps every FP or
    // NEON instruction. GCC inlines memset/memcpy as NEON 128-bit pair
    // stores (stp q, q), so literally nothing C++-runtime-related can
    // run without this enabled. Must happen before .init_array
    // (libsupc++'s eh_alloc ctor calls memset too).
    // ------------------------------------------------------------------
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)      // FPEN = 0b11
    msr     cpacr_el1, x0
    isb

    // ------------------------------------------------------------------
    // Zero BSS — BEFORE setting up MMU. The page table lives in .bss
    // (for 4KB alignment), so if we set up MMU first and THEN zero
    // BSS, we overwrite our own translation tables and the next fetch
    // that misses the TLB faults.
    // ------------------------------------------------------------------
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
bss_loop:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       bss_loop
bss_done:

    // ------------------------------------------------------------------
    // Enable MMU with identity mapping.
    //
    // WHY: MMU off on aarch64 treats all data accesses as Device-nGnRnE
    // memory, which rejects unaligned SIMD pair stores. libgcc's
    // uw_frame_state_for places its `fs` local at sp+0x48 (8-aligned,
    // not 16-aligned) and then GCC emits stp q31, q31, [fs + 800] to
    // inline-zero it. Device memory faults; Normal memory is fine.
    //
    // Map: 4 L1 block entries covering 4GB identity.
    //   [0] 0x00000000–0x3FFFFFFF  Device-nGnRnE   (UART @ 0x09000000)
    //   [1] 0x40000000–0x7FFFFFFF  Normal NC       (our kernel RAM)
    //   [2] 0x80000000–0xBFFFFFFF  Normal NC       (unused)
    //   [3] 0xC0000000–0xFFFFFFFF  Normal NC       (unused)
    //
    // Normal Non-Cacheable, not Write-Back: sidesteps cache bring-up.
    // NC is sufficient to make NEON unaligned work.
    // ------------------------------------------------------------------

    // MAIR_EL1: attr0 = 0x00 (Device-nGnRnE), attr1 = 0x44 (Normal NC)
    mov     x0, #0x4400
    msr     mair_el1, x0

    // Fill the 4 page table entries (rest of the 4KB is zero from the
    // BSS clear above).
    ldr     x1, =_page_table

    // Entry 0: 0x00000000 | 0x401 (AttrIdx=0, AF=1, block+valid)
    mov     x2, #0x401
    str     x2, [x1, #0]

    // Entry 1: 0x40000000 | 0x405 (AttrIdx=1, AF=1, block+valid)
    movz    x2, #0x4000, lsl #16
    movk    x2, #0x0405
    str     x2, [x1, #8]

    // Entry 2: 0x80000000 | 0x405
    movz    x2, #0x8000, lsl #16
    movk    x2, #0x0405
    str     x2, [x1, #16]

    // Entry 3: 0xC0000000 | 0x405
    movz    x2, #0xc000, lsl #16
    movk    x2, #0x0405
    str     x2, [x1, #24]

    // TCR_EL1:
    //   T0SZ = 32 (4GB VA)         bits[5:0]   = 0x20
    //   EPD1 = 1 (disable TTBR1)   bit 23      = 0x800000
    //   IRGN0=ORGN0=SH0=0 (NC walks, non-shareable)
    //   TG0 = 00 (4KB)
    //   IPS = 000 (32-bit PA)      bits[34:32] = 0
    movz    x0, #0x0020
    movk    x0, #0x0080, lsl #16    // 0x00800020
    msr     tcr_el1, x0

    // TTBR0_EL1 = physical address of the page table
    ldr     x0, =_page_table
    msr     ttbr0_el1, x0

    // Invalidate TLB, barriers, enable MMU
    tlbi    vmalle1
    dsb     nsh
    isb

    mrs     x0, sctlr_el1
    orr     x0, x0, #1              // M bit: MMU enable
    msr     sctlr_el1, x0
    isb

    // ------------------------------------------------------------------
    // Call global constructors (.init_array).
    // ------------------------------------------------------------------
    ldr     x19, =__init_array_start
    ldr     x20, =__init_array_end
init_loop:
    cmp     x19, x20
    b.ge    init_done
    ldr     x0, [x19], #8
    blr     x0
    b       init_loop
init_done:

    // Jump to C++ kernel
    bl      kernel_main

    // If kernel_main returns, hang
halt:
    wfe
    b       halt

// ----------------------------------------------------------------------
// Page table storage: one 4KB-aligned L1 table (512 entries). Lives in
// .bss so it takes no file space; zeroed by the BSS loop before we
// write the first 4 entries and enable the MMU.
// ----------------------------------------------------------------------
.section .bss
.align 12                       // 4KB alignment for a page table
.globl _page_table
_page_table:
    .space 4096
