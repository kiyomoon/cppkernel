/* cppkernel linker script — aarch64 bare metal for QEMU virt machine */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .init_array : {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(.init_array.*))
        __init_array_end = .;
    }

    .eh_frame : {
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        /* Sentinel zero terminator — normally provided by crtend.o's
         * contribution to .eh_frame. Without it, libgcc's
         * __register_frame walks off the end of .eh_frame reading
         * garbage as FDE lengths. */
        QUAD(0)
        __eh_frame_end = .;
    }

    .eh_frame_hdr : {
        *(.eh_frame_hdr)
    }

    .gcc_except_table : {
        *(.gcc_except_table .gcc_except_table.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Heap: 512KB after BSS */
    . = ALIGN(4096);
    __heap_start = .;
    . = . + 512K;
    __heap_end = .;

    /* Stack: above heap, grows downward */
    . = ALIGN(4096);
    . = . + 64K;
    __stack_top = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
