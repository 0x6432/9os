# 9os top-level Makefile
ARCH ?= x86_64
BUILD := build/$(ARCH)
CC := clang
LD := ld.lld

# Prefer real C23, fall back to C2x on older clang.
CSTD := $(shell echo 'int x;' | $(CC) -std=c23 -x c -fsyntax-only - 2>/dev/null && echo c23 || echo c2x)

CFLAGS := -std=$(CSTD) -ffreestanding -fno-builtin -nostdlib -fstack-protector-strong \
          -fno-stack-check -fno-lto -fno-pic -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-unused-parameter -O2 -g -MMD -MP \
          -Ikernel/include -Ithird_party/limine -Ikernel/arch/$(ARCH)/include \
          -D__9OS_ARCH_$(ARCH)__

# Build configuration: scheduler policy (rr | mlfq) and CPU count for 'make run'
SCHED ?= rr
SMP ?= 4
# Display for riscv64/aarch64: firmware framebuffers are unusable there, the kernel drives virtio-gpu
QEMU_GPU_x86_64 :=
QEMU_GPU_riscv64 := -device virtio-gpu-pci
QEMU_GPU_aarch64 := -device virtio-gpu-pci
QEMU_GPU ?= $(QEMU_GPU_$(ARCH))
# virtio-input keyboard + tablet (evdev /dev/input/eventN); PS/2 keyboard is also present on x86
QEMU_INPUT ?= -device virtio-keyboard-pci -device virtio-tablet-pci
CONFIG_FLAGS := -DCONFIG_SCHED_$(shell echo $(SCHED) | tr a-z A-Z)=1 -DCONFIG_HARDEN=1
CFLAGS += $(CONFIG_FLAGS)
CONFIG_STAMP := $(BUILD)/config.stamp
$(shell mkdir -p $(BUILD); echo '$(CONFIG_FLAGS)' | cmp -s - $(CONFIG_STAMP) 2>/dev/null || echo '$(CONFIG_FLAGS)' > $(CONFIG_STAMP))

ifeq ($(ARCH),x86_64)
CFLAGS += --target=x86_64-unknown-none-elf -march=x86-64 -mno-red-zone -mcmodel=kernel \
          -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -mno-80387 -mstack-protector-guard=global
LDFLAGS := -m elf_x86_64
endif
ifeq ($(ARCH),riscv64)
CFLAGS += --target=riscv64-unknown-none-elf -march=rv64imac -mabi=lp64 -mcmodel=medany \
          -mno-relax -msmall-data-limit=0
ASFLAGS_ARCH := -march=rv64imafdc
LDFLAGS := -m elf64lriscv --no-relax
endif
ifeq ($(ARCH),aarch64)
CFLAGS += --target=aarch64-unknown-none-elf -mgeneral-regs-only -mcmodel=small
ASFLAGS_ARCH := -march=armv8-a+fp+simd
LDFLAGS := -m aarch64elf
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
          kernel/fs/*.c kernel/net/*.c kernel/acpi/*.c kernel/arch/$(ARCH)/*.c kernel/arch/$(ARCH)/*.S) $(UACPI_SRC)
KOBJ := $(patsubst %,$(BUILD)/%.o,$(KSRC))

KERNEL := $(BUILD)/kernel.elf
ISO := $(BUILD)/9os.iso
INITRAMFS := $(BUILD)/initramfs.cpio

.PHONY: all iso run clean deps
all: $(KERNEL)
iso: $(ISO)

$(KERNEL): $(KOBJ) kernel/arch/$(ARCH)/linker.ld
	$(LD) $(LDFLAGS) $(KOBJ) -o $@

$(BUILD)/%.c.o: %.c $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	@echo "  AS  $<"
	@$(CC) $(CFLAGS) $(ASFLAGS_ARCH) -c $< -o $@

ROOTFS := userland/root-$(ARCH)
$(INITRAMFS): $(shell find $(ROOTFS) -type f 2>/dev/null)
	@mkdir -p $(BUILD) $(ROOTFS)
	(cd $(ROOTFS) && find . | cpio -o -H newc --quiet) > $@

LIMINE := third_party/limine-bin
$(ISO): $(KERNEL) $(INITRAMFS) limine.conf
	@test -x $(ROOTFS)/bin/busybox || { echo "error: $(ROOTFS) is empty - run 'ARCH=$(ARCH) userland/build-all.sh' first" >&2; exit 1; }
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/limine $(BUILD)/iso/EFI/BOOT
	cp $(KERNEL) $(INITRAMFS) $(BUILD)/iso/boot/
	cp limine.conf $(BUILD)/iso/boot/limine/
	cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTAA64.EFI $(LIMINE)/BOOTRISCV64.EFI $(BUILD)/iso/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot \
	    -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(BUILD)/iso -o $@ 2>/dev/null
	if [ $(ARCH) = x86_64 ]; then $(LIMINE)/limine bios-install $@ 2>/dev/null; fi

# UEFI firmware for riscv64/aarch64: searched in the usual distro locations (override with FW_CODE=...)
FW_DIRS := $(dir $(shell which qemu-system-$(ARCH) 2>/dev/null))../share/qemu /usr/share/qemu /usr/share/edk2/riscv \
           /usr/share/edk2/aarch64 /usr/share/qemu-efi-aarch64 /usr/share/AAVMF /usr/share/qemu-efi-riscv64 /usr/share/edk2-ovmf /usr/lib/u-boot/qemu-riscv64_smode
FW_NAMES_riscv64 := edk2-riscv-code.fd RISCV_VIRT_CODE.fd
FW_NAMES_aarch64 := edk2-aarch64-code.fd QEMU_EFI.fd AAVMF_CODE.fd
FW_SIZE_riscv64 := 33554432
FW_SIZE_aarch64 := 67108864
FW_CODE ?= $(firstword $(wildcard $(foreach d,$(FW_DIRS),$(foreach n,$(FW_NAMES_$(ARCH)),$(d)/$(n)))))
# CPU models with the M27 hardening features (SMEP/SMAP; PAN needs ARMv8.1+)
QEMU_CPU_x86_64 := -cpu qemu64,+smep,+smap,+rdrand
QEMU_CPU_riscv64 :=
QEMU_CPU_aarch64 := -cpu cortex-a76
QEMU_CPU ?= $(QEMU_CPU_$(ARCH))
QEMU_x86_64 := qemu-system-x86_64 -M q35 $(QEMU_CPU) -m 512M -smp $(SMP) -serial stdio -no-reboot $(QEMU_INPUT) -cdrom $(ISO)
QEMU_riscv64 := qemu-system-riscv64 -M virt $(QEMU_CPU) -m 512M -smp $(SMP) -serial stdio -no-reboot $(QEMU_GPU) $(QEMU_INPUT) \
    -drive if=pflash,unit=0,format=raw,readonly=on,file=$(BUILD)/fw-code.fd \
    -drive if=pflash,unit=1,format=raw,file=$(BUILD)/fw-vars.fd \
    -drive if=none,id=cd,format=raw,media=cdrom,file=$(ISO) -device virtio-scsi-pci -device scsi-cd,drive=cd
QEMU_aarch64 := qemu-system-aarch64 -M virt $(QEMU_CPU) -m 512M -smp $(SMP) -serial stdio -no-reboot $(QEMU_GPU) $(QEMU_INPUT) \
    -drive if=pflash,unit=0,format=raw,readonly=on,file=$(BUILD)/fw-code.fd \
    -drive if=pflash,unit=1,format=raw,file=$(BUILD)/fw-vars.fd \
    -drive if=none,id=cd,format=raw,media=cdrom,file=$(ISO) -device virtio-scsi-pci -device scsi-cd,drive=cd

.PHONY: firmware
firmware:
ifneq ($(ARCH),x86_64)
	@test -n "$(FW_CODE)" || { echo "no UEFI firmware for $(ARCH) found; install edk2/qemu-efi or set FW_CODE=" >&2; exit 1; }
	@mkdir -p $(BUILD)
	@test -f $(BUILD)/fw-code.fd || { cp $(FW_CODE) $(BUILD)/fw-code.fd; \
	    [ $$(stat -c %s $(BUILD)/fw-code.fd) -ge $(FW_SIZE_$(ARCH)) ] || truncate -s $(FW_SIZE_$(ARCH)) $(BUILD)/fw-code.fd; }
	@test -f $(BUILD)/fw-vars.fd || truncate -s $$(stat -c %s $(BUILD)/fw-code.fd) $(BUILD)/fw-vars.fd
endif

run: $(ISO) firmware
	$(QEMU_$(ARCH)) $(QEMUFLAGS)

deps:
	./scripts/fetch-deps.sh

clean:
	rm -rf build

-include $(KOBJ:.o=.d)
