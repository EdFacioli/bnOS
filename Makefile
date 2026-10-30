CC      := gcc
LD      := ld
BUILD   := build

CFLAGS  := -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
           -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone
ASFLAGS := -m32
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack --build-id=none

OBJS := $(BUILD)/boot.o $(BUILD)/gdt_flush.o $(BUILD)/isr.o \
        $(BUILD)/kernel.o $(BUILD)/vga.o $(BUILD)/gdt.o $(BUILD)/idt.o \
        $(BUILD)/pic.o $(BUILD)/keyboard.o $(BUILD)/pmm.o $(BUILD)/paging.o \
        $(BUILD)/shell.o $(BUILD)/lib.o

all: $(BUILD)/kernel.elf

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

run: $(BUILD)/kernel.elf
	qemu-system-i386 -kernel $(BUILD)/kernel.elf

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
