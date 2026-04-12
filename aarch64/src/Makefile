# cppkernel — aarch64 freestanding kernel
#
# Build:
#     make
#
# Run:
#     qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M -nographic \
#         -kernel build/kernel.elf
# (Ctrl-A, X to quit QEMU.)
#
# Requirements: an aarch64 g++ from GCC 14 or later, plus qemu.
# On Ubuntu 24.04 arm64:
#     sudo apt install g++-14 qemu-system-arm
# then build with:
#     make CXX=g++-14
#
# A Dockerfile that sets this up is in the parent directory.

CXX ?= g++
AS  := $(CXX)
LD  := $(CXX)

CXXFLAGS := -std=c++17 \
            -ffreestanding -nostdlib \
            -fasynchronous-unwind-tables \
            -Wall -Wextra -O2 \
            -mcpu=cortex-a72

# Link notes (see the article's "Seven Walls" section for why):
#
#   - Normal -lgcc_eh is enough. Do NOT use --whole-archive — it
#     drags in emutls, heap-trampoline, SME/LSE detection, and
#     unwind-dw2-fde-dip, each of which pulls in more undefined
#     symbols (mmap, pthread_cond_*, _dl_find_object, getauxval).
#     Plain -lgcc_eh works because __cxa_throw has a direct symbol
#     reference to _Unwind_RaiseException.
#
#   - The order below matters: object files before -lsupc++. This
#     way, cxxrt.o's definition of __cxa_get_globals resolves the
#     reference from libsupc++'s eh_throw.o before eh_globals.o
#     gets a chance to be pulled from the archive. See the Wall 5
#     section of the article.
#
# We don't pass explicit -L flags for libsupc++ / libgcc_eh. The g++
# driver knows where its own libraries live and adds them to the
# search path even under -nostdlib.
LDFLAGS := -nostdlib -T linker.ld
LDLIBS  := -lsupc++ -lgcc_eh -lgcc

BUILD  := build
KERNEL := $(BUILD)/kernel.elf

SRCS_CXX := kernel.cpp cxxrt.cpp
SRCS_ASM := boot.S

OBJS := $(SRCS_CXX:%.cpp=$(BUILD)/%.o) $(SRCS_ASM:%.S=$(BUILD)/%.o)

.PHONY: all clean run

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

run: $(KERNEL)
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 128M -nographic \
		-kernel $(KERNEL)

clean:
	rm -rf $(BUILD)
