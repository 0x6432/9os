/* Inert x86-64 host stand-ins for unrelated boot operations in kmain.
 * Keep explicit: a newly referenced symbol must fail linking, not silently stub. */
.macro noop name
.globl \name
.type \name,@function
\name:
    xor %eax, %eax
    ret
.endm
noop acpi_early_init
noop acpi_late_init
noop arch_early_init
noop arch_init
noop boot_check
noop devices_init
noop drm_init
noop evdev_register_chrdev
noop ext2_init
noop fbcon_init
noop fbcon_vt_init
noop fbdev_init
noop harden_init
noop input_init
noop irq_work_enable
noop loop_init
noop net_init
noop pci_init
noop pmm_cache_selftest
noop pmm_enable_cpu_caches
noop pmm_init
noop pmm_selftest
noop procfs_create_root
noop sched_init
noop slab_cpu_selftest
noop slab_enable_cpu_caches
noop slab_init
noop slab_selftest
noop smp_init
noop stack_guard_init
noop sysfs_create_root
noop uinput_init
noop vfs_init
noop vfs_mkdir_at
noop vfs_mount
noop virtio_blk_init
noop virtio_gpu_init
noop virtio_input_init
noop virtio_net_init
noop virtio_snd_init
noop vmm_init
noop vt_late_init
noop writeback_init
/* Unexpected scheduler-selftest calls must not silently succeed. */
.globl thread_create
.type thread_create,@function
thread_create: ud2
.globl time_ns
.type time_ns,@function
time_ns: ud2
.section .note.GNU-stack,"",@progbits
