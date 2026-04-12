#pragma once
// PL011 UART driver for QEMU virt machine
// Base address: 0x0900_0000

#include <cstdint>

namespace uart {

namespace {
    constexpr std::uintptr_t BASE = 0x0900'0000ULL;
    constexpr std::uint32_t DR   = 0x000;
    constexpr std::uint32_t FR   = 0x018;
    constexpr std::uint32_t FR_TXFF = (1 << 5);

    inline volatile std::uint32_t& reg(std::uint32_t offset) {
        return *reinterpret_cast<volatile std::uint32_t*>(BASE + offset);
    }
}

inline void putc(char c) {
    while (reg(FR) & FR_TXFF)
        ;
    reg(DR) = c;
}

inline void puts(const char* s) {
    while (*s) {
        if (*s == '\n') putc('\r');
        putc(*s++);
    }
}

inline void put_hex(std::uint64_t val) {
    puts("0x");
    constexpr char hex[] = "0123456789abcdef";
    bool started = false;
    for (int i = 60; i >= 0; i -= 4) {
        auto nibble = (val >> i) & 0xF;
        if (nibble != 0) started = true;
        if (started) putc(hex[nibble]);
    }
    if (!started) putc('0');
}

} // namespace uart
