/* GPL-3.0-or-later. CH32H415 optional BOOT-button debug handoff. */
#pragma once
#include "drivers/time.h"

void ch32h415BootSdiSystemTask(timeUs_t nowUs);
void ch32h415UsbReleaseForDebug(void);
