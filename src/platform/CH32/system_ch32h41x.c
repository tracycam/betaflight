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

#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#include "drivers/exti.h"
#include "drivers/nvic.h"
#include "drivers/system.h"
#include "drivers/persistent.h"
#include "drivers/usb_io.h"

#ifndef USE_VCP
bool usbCableIsInserted(void)
{
    return false;
}

bool usbCableIsActive(void)
{
    return false;
}
#endif

#define CH32_FLASH_RESET_WAIT_LIMIT 0x02000000U

static void ch32WaitFlashIdleBeforeReset(void)
{
    for (uint32_t timeout = CH32_FLASH_RESET_WAIT_LIMIT;
         timeout != 0U && (FLASH->STATR & (FLASH_STATR_BSY | FLASH_STATR_WRBSY)) != 0U;
         timeout--) {
    }
}

void systemReset(void)
{
    __disable_irq();
    ch32WaitFlashIdleBeforeReset();
    asm volatile ("fence iorw, iorw" ::: "memory");
    NVIC_SystemReset();
}

void systemResetToBootloader(bootloaderRequestType_e requestType)
{
    (void)requestType;
    // Protocol entry points reject this operation. Keep a normal reset as a
    // fallback for direct callers until the paired bootloader supports it.
    persistentObjectWrite(PERSISTENT_OBJECT_RESET_REASON, RESET_NONE);
    asm volatile ("fence iorw, iorw" ::: "memory");
    systemReset();
}

static void checkForBootLoaderRequest(void)
{
    const uint32_t bootloaderRequest = persistentObjectRead(PERSISTENT_OBJECT_RESET_REASON);
    if (bootloaderRequest == RESET_BOOTLOADER_REQUEST_ROM || bootloaderRequest == RESET_BOOTLOADER_REQUEST_FLASH) {
        // Ignore requests left by older firmware. WCH ROM ISP is not the
        // H415 USB DFU bootloader; never change FLASH_STATR_BOOT_MODE here.
        persistentObjectWrite(PERSISTENT_OBJECT_RESET_REASON, RESET_NONE);
    }
}

void enableGPIOPowerUsageAndNoiseReductions(void)
{
    RCC_HBPeriphClockCmd(
        RCC_HBPeriph_DMA1 |
        RCC_HBPeriph_DMA2 |
        RCC_HBPeriph_PIOC,
        ENABLE
    );
#if defined(USE_VCP) || defined(USE_USB_MSC)
    RCC_HBPeriphClockCmd(RCC_HBPeriph_USBHS, ENABLE);
#endif
    RCC_HB1PeriphClockCmd(
        RCC_HB1Periph_TIM2 |
        RCC_HB1Periph_TIM3 |
        RCC_HB1Periph_TIM4 |
        RCC_HB1Periph_TIM5 |
        RCC_HB1Periph_TIM6 |
        RCC_HB1Periph_TIM7 |
        RCC_HB1Periph_USART6 |
        RCC_HB1Periph_USART7 |
        RCC_HB1Periph_USART8 |
        RCC_HB1Periph_LPTIM1 |
        RCC_HB1Periph_LPTIM2 |
        RCC_HB1Periph_WWDG |
        RCC_HB1Periph_SPI2 |
        RCC_HB1Periph_SPI3 |
        RCC_HB1Periph_SPI4 |
        RCC_HB1Periph_USART2 |
        RCC_HB1Periph_USART3 |
        RCC_HB1Periph_USART4 |
        RCC_HB1Periph_USART5 |
        RCC_HB1Periph_I2C1 |
        RCC_HB1Periph_I2C2 |
        RCC_HB1Periph_CAN3 |
        RCC_HB1Periph_CAN1 |
        RCC_HB1Periph_CAN2 |
        RCC_HB1Periph_BKP |
        RCC_HB1Periph_PWR |
        RCC_HB1Periph_DAC |
        RCC_HB1Periph_I2C3 |
        RCC_HB1Periph_SWPMI,
        ENABLE
    );
    RCC_HB2PeriphClockCmd(
        RCC_HB2Periph_AFIO |
        RCC_HB2Periph_GPIOA |
        RCC_HB2Periph_GPIOB |
        RCC_HB2Periph_GPIOC |
        RCC_HB2Periph_GPIOD |
        RCC_HB2Periph_GPIOE |
        RCC_HB2Periph_GPIOF |
        RCC_HB2Periph_ADC1 |
        RCC_HB2Periph_ADC2 |
        RCC_HB2Periph_TIM1 |
        RCC_HB2Periph_SPI1 |
        RCC_HB2Periph_TIM8 |
        RCC_HB2Periph_USART1 |
        RCC_HB2Periph_I2C4 |
        RCC_HB2Periph_SDIO |
        RCC_HB2Periph_TIM9 |
        RCC_HB2Periph_TIM10 |
        RCC_HB2Periph_TIM11 |
        RCC_HB2Periph_TIM12,
        ENABLE
    );
}

bool isMPUSoftReset(void)
{
    if (cachedResetFlags & RCC_SFTRSTF) {
        return true;
    } else {
        return false;
    }
}

uint32_t SysTick_Config(uint32_t ticks)
{
    if ((ticks - 1UL) > 0xFFFFFFFFUL) {
        return 1UL;                                                     /* Reload value impossible */
    }
    SysTick0->ISR &= ~(1U << 1);                                        /* Clear flag */
    SysTick1->CMP = (uint32_t)(ticks - 1UL);                            /* Set reload register */
    SysTick1->CNT = 0UL;                                                /* Load the SysTick counter value */
    SysTick1->CTLR = (1U << 6) | (1U << 4) | (1U << 3) | (1U << 2) | (1U << 1) | (1U << 0);
    NVIC_SetPriority(SysTick1_IRQn, NVIC_BUILD_PRIORITY(3, 0));         /* Lowest preemption group */
    NVIC_EnableIRQ(SysTick1_IRQn);
    return 0UL;                                                         /* Function successful */
}

void systemInit(void)
{
    SystemAndCoreClockUpdate();

    persistentObjectInit();

    checkForBootLoaderRequest();

    // Cache RCC->RSTSCKR value to use it in isMPUSoftReset() and others
    cachedResetFlags = RCC->RSTSCKR;

    RCC_HBPeriphClockCmd(RCC_HBPeriph_USBHS | RCC_HBPeriph_OTG_FS, DISABLE);

    RCC_ClearFlag();
    enableGPIOPowerUsageAndNoiseReductions();

    cycleCounterInit();
    SysTick_Config(HCLKClock / 1000);

    SWPMI->OR |= 1; // Disable SWPMI, enable GPIO input
}
