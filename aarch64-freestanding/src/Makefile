CXX      := g++
AS       := g++
LD       := g++

CXXFLAGS := -std=c++17 \
            -ffreestanding -nostdlib \
            -fasynchronous-unwind-tables \
            -Wall -Wextra -O2 \
            -mcpu=cortex-a72

# M3: link GCC's exception runtime statically.
# libsupc++ provides __cxa_throw, __gxx_personality_v0, etc.
# libgcc_eh provides _Unwind_RaiseException and DWARF CFI unwinder.
#
# Link notes (learned the hard way — see DEVLOG):
#   - Normal -lgcc_eh is enough. __cxa_throw directly references
#     _Unwind_RaiseException so the linker pulls the unwinder in.
#     --whole-archive drags in unrelated libgcc_eh objects (emutls,
#     heap-trampoline, SME/LSE detection) that need mmap, pthread,
#     getauxval, etc — a freestanding mess.
#   - cxxrt.cpp defines __cxa_get_globals / __cxa_get_globals_fast
#     BEFORE libsupc++.a on the link line, so eh_globals.o never
#     gets pulled in (it would use __thread TLS via TPIDR_EL0 which
#     we have not set up).
# GCC built with --disable-hosted-libstdcxx produces a freestanding
# libsupc++.a that needs only malloc/free/calloc + abort from us.
# No __cxa_get_globals override, no secure_getenv stubs, no verbose
# terminate handler, no cp-demangle, no pthread. Just allocator + panic.
GCC_LIB_DIR := /opt/gcc-16/lib/gcc/aarch64-unknown-linux-gnu/16.0.1
SUPCXX_DIR  := /opt/gcc-16/lib64

LDFLAGS  := -nostdlib -T linker.ld \
            -L$(SUPCXX_DIR) -L$(GCC_LIB_DIR)

LDLIBS   := -lsupc++ -lgcc_eh -lgcc

BUILD    := build
KERNEL   := $(BUILD)/kernel.elf

SRCS_CXX := kernel.cpp cxxrt.cpp
SRCS_ASM := boot.S

OBJS     := $(SRCS_CXX:%.cpp=$(BUILD)/%.o) $(SRCS_ASM:%.S=$(BUILD)/%.o)

.PHONY: all clean

all: $(KERNEL)

$(KERNEL): $(OBJS) linker.ld | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@echo ""
	@echo "Built: $@"

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	$(AS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
