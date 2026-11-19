#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>
#include <stdbool.h>

extern bool bios_hle_enabled;

void bios_init();
void bios_direct_boot();
bool bios_swi(uint8_t comment);

#endif
