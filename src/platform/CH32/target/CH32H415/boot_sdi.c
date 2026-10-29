/* GPL-3.0-or-later. The H415 BOOT button is active-high on PC8. */
#include "platform.h"

#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
#include <stdbool.h>
#include <stdint.h>
#include "scheduler/scheduler.h"
#include "target/CH32H415/boot_sdi.h"

#define BOOT_SDI_HOLD_US 5000000u

void ch32h415BootSdiSystemTask(timeUs_t nowUs)
{
    static bool initialized, holding, switched;
    static timeUs_t pressedAt;

    taskSystemLoad(nowUs);
    if (switched) {
        return;
    }
    if (!initialized) {
        GPIO_InitTypeDef gpio = { 0 };
        gpio.GPIO_Pin = GPIO_Pin_8;
        gpio.GPIO_Speed = GPIO_Speed_Medium;
        gpio.GPIO_Mode = GPIO_Mode_IPD;
        GPIO_Init(GPIOC, &gpio);
        initialized = true;
    }
    if (!(GPIOC->INDR & GPIO_Pin_8)) {
        holding = false;
    } else if (!holding) {
        holding = true;
        pressedAt = nowUs;
    } else if ((uint32_t)(nowUs - pressedAt) >= BOOT_SDI_HOLD_US) {
        ch32h415UsbReleaseForDebug();
        switched = true;
    }
}
#endif
