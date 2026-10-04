#pragma once
#include <kernel/types.h>
void acpi_early_init(void);   /* table access only */
void acpi_late_init(void);    /* namespace load + initialize (needs interrupts) */
void *acpi_find_table(const char *sig);
void acpi_poweroff(void);
void acpi_reboot(void);
