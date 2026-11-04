/* GPL-3.0-or-later. CH32H415 optional BOOT-button debug handoff. */
#pragma once
#include <stdbool.h>
#include "drivers/time.h"

void ch32h415BootSdiPoll(timeUs_t nowUs);
void ch32h415BootSdiSystemTask(timeUs_t nowUs);
void ch32h415UsbMarkCoreInitialized(void);
bool ch32h415UsbIsReleasedForDebug(void);
void ch32h415UsbReleaseForDebug(void);
