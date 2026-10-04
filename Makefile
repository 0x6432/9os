# 9os top-level Makefile
ARCH ?= x86_64
BUILD := build/$(ARCH)
CC := clang
LD := ld.lld

# Prefer real C23, fall back to C2x on older clang.
CSTD := $(shell echo 'int x;' | $(CC) -std=c23 -x c -fsyntax-only - 2>/dev/null && echo c23 || echo c2x)

CFLAGS := -std=$(CSTD) -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
          -fno-stack-check -fno-lto -fno-pic -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-unused-parameter -O2 -g -MMD -MP \
          -Ikernel/include -Ithird_party/limine -Ikernel/arch/$(ARCH)/include \
          -DLIMINE_API_REVISION=3 -D__9OS_ARCH_$(ARCH)__

ifeq ($(ARCH),x86_64)
CFLAGS += --target=x86_64-unknown-none-elf -march=x86-64 -mno-red-zone -mcmodel=kernel \
          -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -mno-80387
LDFLAGS := -m elf_x86_64
endif

LDFLAGS += -nostdlib -static -z max-page-size=0x1000 --no-dynamic-linker \
           -T kernel/arch/$(ARCH)/linker.ld

# uACPI (fetched by scripts/fetch-deps.sh)
UACPI := third_party/uACPI
ifneq ($(wildcard $(UACPI)/source),)
CFLAGS += -I$(UACPI)/include -DUACPI_SIZED_FREES -DHAVE_UACPI
UACPI_SRC := $(wildcard $(UACPI)/source/*.c)
endif

KSRC := $(wildcard kernel/core/*.c kernel/mm/*.c kernel/lib/*.c kernel/drivers/*.c \
          kernel/fs/*.c kernel/acpi/*.c kernel/arch/$(ARCH)/*.c kernel/arch/$(ARCH)/*.S) $(UACPI_SRC)
KOBJ := $(patsubst %,$(BUILD)/%.o,$(KSRC))

KERNEL := $(BUILD)/kernel.elf
ISO := $(BUILD)/9os.iso
INITRAMFS := $(BUILD)/initramfs.cpio

.PHONY: all iso run clean deps
all: $(KERNEL)
iso: $(ISO)

$(KERNEL): $(KOBJ) kernel/arch/$(ARCH)/linker.ld
	$(LD) $(LDFLAGS) $(KOBJ) -o $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(INITRAMFS): $(shell find userland/root -type f 2>/dev/null)
	@mkdir -p $(BUILD) userland/root
	(cd userland/root && find . | cpio -o -H newc --quiet) > $@

LIMINE := third_party/limine-bin
$(ISO): $(KERNEL) $(INITRAMFS) limine.conf
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/limine $(BUILD)/iso/EFI/BOOT
	cp $(KERNEL) $(INITRAMFS) $(BUILD)/iso/boot/
	cp limine.conf $(BUILD)/iso/boot/limine/
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTAA64.EFI $(LIMINE)/BOOTRISCV64.EFI $(BUILD)/iso/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot \
	    -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(BUILD)/iso -o $@ 2>/dev/null
	$(LIMINE)/limine bios-install $@ 2>/dev/null

QEMU_x86_64 := qemu-system-x86_64 -M q35 -m 512M -serial stdio -no-reboot -no-shutdown
run: $(ISO)
	$(QEMU_$(ARCH)) -cdrom $(ISO) $(QEMUFLAGS)

deps:
	./scripts/fetch-deps.sh

clean:
	rm -rf build

-include $(KOBJ:.o=.d)
