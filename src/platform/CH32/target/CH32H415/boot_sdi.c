/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

/* The CH32H415 BOOT button is active-high on PC8. */

#include "platform.h"

#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
#include <stdbool.h>
#include <stdint.h>

#include "platform/usb_sdi_handoff.h"
#include "scheduler/scheduler.h"

#define BOOT_SDI_HOLD_US 5000000u

void ch32h415BootSdiPoll(timeUs_t nowUs)
{
    static bool initialized, holding, switched;
    static timeUs_t pressedAt;

    if (switched) {
        return;
    }
    if (!initialized) {
        GPIO_InitTypeDef gpio = { 0 };
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_GPIOC, ENABLE);
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

void ch32h415BootSdiSystemTask(timeUs_t nowUs)
{
    taskSystemLoad(nowUs);
    ch32h415BootSdiPoll(nowUs);
}
#endif
