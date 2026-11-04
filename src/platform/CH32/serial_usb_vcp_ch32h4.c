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

#include <stdint.h>
#include <stdbool.h>

#include "platform.h"

#ifdef USE_VCP

#include "build/build_config.h"
#include "build/atomic.h"

#include "common/utils.h"

#include "drivers/io.h"
#include "drivers/usb_io.h"

#include "pg/usb.h"

#include "ch32_debug.h"
#include "cdc_vcp_ch32h41x.h"
#include "usb_ch32h41x.h"
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usb_ch32h41x_usbhs_reg.h"

#include "drivers/time.h"
#include "drivers/serial.h"
#include "drivers/serial_usb_vcp.h"
#include "drivers/nvic.h"

#define USB_TIMEOUT  50

static vcpPort_t vcpPort = { 0 };
#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
#include "target/CH32H415/boot_sdi.h"
static bool usbCoreInitialized;
static bool usbReleasedForDebug;
#endif

// #define APP_RX_DATA_SIZE  2048
#define APP_TX_DATA_SIZE  8192

#define APP_TX_BLOCK_SIZE 512

// volatile uint8_t UserRxBuffer[APP_RX_DATA_SIZE];/* Received Data over USB are stored in this buffer */
volatile uint8_t UserTxBuffer[APP_TX_DATA_SIZE];/* Received Data over UART (CDC interface) are stored in this buffer */
uint32_t BuffLength;

/* Increment this pointer or roll it back to start address when data are received over USART */
volatile uint32_t UserTxBufPtrIn = 0;
/* Increment this pointer or roll it back to start address when data are sent over USB */
volatile uint32_t UserTxBufPtrOut = 0;
static volatile uint32_t lastBuffsize = 0;
static volatile bool usbZlpPending;
static volatile bool usbTxStalled;
static volatile bool usbHostPortOpen;

// volatile uint32_t APP_Rx_ptr_out = 0;
// volatile uint32_t APP_Rx_ptr_in = 0;
// static uint8_t APP_Rx_Buffer[APP_RX_DATA_SIZE];


// #define  CDC_POLLING_INTERVAL 5



static void (*ctrlLineStateCb)(void *context, uint16_t ctrlLineState);
static void *ctrlLineStateCbContext;
static void (*baudRateCb)(void *context, uint32_t baud);
static void *baudRateCbContext;

void CDC_SetBaudRateCb(void (*cb)(void *context, uint32_t baud), void *context)
{
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        baudRateCbContext = context;
        baudRateCb = cb;
    }
}

void CDC_SetCtrlLineStateCb(void (*cb)(void *context, uint16_t ctrlLineState), void *context)
{
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        ctrlLineStateCbContext = context;
        ctrlLineStateCb = cb;
    }
}

void usbVcpBaudRateChanged(uint32_t baud)
{
    if (baudRateCb) {
        baudRateCb(baudRateCbContext, baud);
    }
}

void usbVcpControlLineStateChanged(uint16_t state)
{
    usbHostPortOpen = (state & CTRL_LINE_STATE_DTR) != 0;
    if (usbHostPortOpen) {
        usbTxStalled = false;
    }
    if (ctrlLineStateCb) {
        ctrlLineStateCb(ctrlLineStateCbContext, state);
    }
}


uint8_t usbIsConnected(void)
{
#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
    if (usbReleasedForDebug) {
        return false;
    }
#endif
    return ((usbd_cdc_info != USBD_EVENT_DISCONNECTED) && (usbd_cdc_info != USBD_EVENT_UNKNOWN));
}

uint8_t usbIsConfigured(void)
{
#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
    if (usbReleasedForDebug) {
        return false;
    }
#endif
    return (usbd_cdc_info == USBD_EVENT_CONFIGURED);
}

uint8_t usbVcpIsConnected(void)
{
    return usbIsConnected();
}

uint8_t usbVcpIsActive(void)
{
    return usbIsConfigured();
}

void usbVcpResetState(void)
{
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        UserTxBufPtrIn = 0;
        UserTxBufPtrOut = 0;
        lastBuffsize = 0;
        usbZlpPending = false;
        usbTxStalled = false;
        usbHostPortOpen = false;
    }
}

static void usbVcpKickTx(void)
{
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        if (ep_tx_busy_flag == 0 && usbIsConfigured()) {
            if (lastBuffsize) {
                uint32_t nextOut = UserTxBufPtrOut + lastBuffsize;
                if (nextOut >= APP_TX_DATA_SIZE) {
                    nextOut -= APP_TX_DATA_SIZE;
                }
                if ((lastBuffsize % CDC_MAX_MPS) == 0 && nextOut == UserTxBufPtrIn) {
                    usbZlpPending = true;
                }
                UserTxBufPtrOut = nextOut;
                lastBuffsize = 0;
            }
            if (usbZlpPending && UserTxBufPtrOut != UserTxBufPtrIn) {
                usbZlpPending = false;
            }
            if (usbZlpPending) {
                if (usb_vcp_send_data(0, (uint8_t *)&UserTxBuffer[UserTxBufPtrOut], 0) == 0) {
                    usbZlpPending = false;
                }
            } else if (UserTxBufPtrOut != UserTxBufPtrIn) {
                uint32_t buffsize = (UserTxBufPtrOut > UserTxBufPtrIn)
                    ? (APP_TX_DATA_SIZE - UserTxBufPtrOut)
                    : (UserTxBufPtrIn - UserTxBufPtrOut);
                if (buffsize > APP_TX_BLOCK_SIZE) {
                    buffsize = APP_TX_BLOCK_SIZE;
                }
                if (usb_vcp_send_data(0, (uint8_t *)&UserTxBuffer[UserTxBufPtrOut], (uint16_t)buffsize) == 0) {
                    lastBuffsize = buffsize;
                }
            }
        }
    }
}

void usbVcpTxDoneISR(void)
{
    usbTxStalled = false;
    usbVcpKickTx();
}

void usb_int_rxsof_handler(void)
{
    usbVcpKickTx();
}

uint32_t CDC_Send_FreeBytes(void)
{
    uint32_t freeBytes;

    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        freeBytes =
            ((UserTxBufPtrOut - UserTxBufPtrIn) + (-((int)(UserTxBufPtrOut <= UserTxBufPtrIn)) & APP_TX_DATA_SIZE)) - 1;
    }

    return freeBytes;
}

uint32_t CDC_Send_DATA(const uint8_t *ptrBuffer, uint32_t sendLength)
{
    if (!usbIsConfigured() || !usbVcpIsConnected()) {
        return 0;
    }

    const timeUs_t startedAt = micros();
    uint32_t offset = 0;

    while (offset < sendLength) {
        uint32_t chunk = 0;
        ATOMIC_BLOCK(NVIC_PRIO_USB) {
            const uint32_t freeBytes =
                ((UserTxBufPtrOut - UserTxBufPtrIn) + (-((int)(UserTxBufPtrOut <= UserTxBufPtrIn)) & APP_TX_DATA_SIZE)) - 1;
            chunk = sendLength - offset;
            if (chunk > freeBytes) {
                chunk = freeBytes;
            }
            for (uint32_t i = 0; i < chunk; i++) {
                UserTxBuffer[UserTxBufPtrIn] = ptrBuffer[offset + i];
                UserTxBufPtrIn++;
                if (UserTxBufPtrIn >= APP_TX_DATA_SIZE) {
                    UserTxBufPtrIn = 0;
                }
            }
        }

        if (chunk > 0) {
            offset += chunk;
            usbVcpKickTx();
        } else {
            usbVcpKickTx();
            if (usbTxStalled || !usbIsConfigured() || !usbVcpIsConnected()) {
                return offset;
            }
            if (micros() - startedAt >= USB_TIMEOUT * 1000) {
                usbTxStalled = true;
                return offset;
            }
            delay(1);
        }
    }

    return sendLength;
}



static void usbVcpSetBaudRate(serialPort_t *instance, uint32_t baudRate)
{
    UNUSED(instance);
    UNUSED(baudRate);
}

static void usbVcpSetMode(serialPort_t *instance, portMode_e mode)
{
    UNUSED(instance);
    UNUSED(mode);
}

static void usbVcpSetCtrlLineStateCb(serialPort_t *instance, void (*cb)(void *context,
                                                                        uint16_t ctrlLineState), void *context)
{
    UNUSED(instance);
    CDC_SetCtrlLineStateCb((void (*)(void *context, uint16_t ctrlLineState))cb, context);
}

static void usbVcpSetBaudRateCb(serialPort_t *instance, void (*cb)(serialPort_t *context,
                                                                   uint32_t baud), serialPort_t *context)
{
    UNUSED(instance);
    CDC_SetBaudRateCb((void (*)(void *context, uint32_t baud))cb, (void *)context);
}

static bool isUsbVcpTransmitBufferEmpty(const serialPort_t *instance)
{
    UNUSED(instance);
    bool empty;
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        empty = !usbIsConfigured() || !usbHostPortOpen ||
            (!vcpPort.txAt && UserTxBufPtrOut == UserTxBufPtrIn && !lastBuffsize && !usbZlpPending && !ep_tx_busy_flag);
    }
    return empty;
}

static uint32_t usbVcpAvailable(const serialPort_t *instance)
{
    UNUSED(instance);
#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
    if (usbReleasedForDebug) {
        return 0;
    }
#endif

//     uint32_t available = APP_Rx_ptr_in-APP_Rx_ptr_out;

//     // Return the sum of the bytes in the APP_Rx_Buffer buffer and those received by the VCP driver
//   if(ep_rx_finish == 1)
//     {
//         available += ep_rx_length;
//     }

//     return available;

    return usb_vcp_rx_available( );
}

static uint8_t usbVcpRead(serialPort_t *instance)
{
    UNUSED(instance);

//    if ((APP_Rx_ptr_in == 0) || (APP_Rx_ptr_out == APP_Rx_ptr_in))
//    {
//         APP_Rx_ptr_out = 0;
//         APP_Rx_ptr_in = usb_vcp_get_rx_data(0, APP_Rx_Buffer);
//         if(APP_Rx_ptr_in == 0) {
//             // We've drained the input buffer
//             return 0;
//         }
//     }
//     return APP_Rx_Buffer[APP_Rx_ptr_out++];

    uint8_t buf[1];
    if (usb_vcp_get_rx_data(0, buf, 1)) {
        return buf[0];
    }
    return 0;
}

static void usbVcpWriteBuf(serialPort_t *instance, const void *data, int count)
{
    UNUSED(instance);

    if (!(usbIsConnected() && usbIsConfigured())) {
        return;
    }

    uint32_t start = millis();
    const uint8_t *p = data;
    while (count > 0) {
        uint32_t txed = CDC_Send_DATA(p, count);
        count -= txed;
        p += txed;

        if (txed == 0 || millis() - start >= USB_TIMEOUT) {
            break;
        }
    }
}

static bool usbVcpFlush(vcpPort_t *port)
{
    uint32_t count = port->txAt;
    port->txAt = 0;

    if (count == 0) {
        return true;
    }

    if (!usbIsConnected() || !usbIsConfigured()) {
        return false;
    }

    uint32_t start = millis();
    uint8_t *p = port->txBuf;
    while (count > 0) {
        uint32_t txed = CDC_Send_DATA(p, count);
        count -= txed;
        p += txed;

        if (txed == 0 || millis() - start >= USB_TIMEOUT) {
            break;
        }
    }
    return count == 0;
}
static void usbVcpWrite(serialPort_t *instance, uint8_t c)
{
    vcpPort_t *port = container_of(instance, vcpPort_t, port);

    port->txBuf[port->txAt++] = c;
    if (!port->buffering || port->txAt >= ARRAYLEN(port->txBuf)) {
        usbVcpFlush(port);
    }
}

static void usbVcpBeginWrite(serialPort_t *instance)
{
    vcpPort_t *port = container_of(instance, vcpPort_t, port);
    port->buffering = true;
}

static uint32_t usbTxBytesFree(const serialPort_t *instance)
{
    UNUSED(instance);
    return CDC_Send_FreeBytes();
}

static void usbVcpEndWrite(serialPort_t *instance)
{
    vcpPort_t *port = container_of(instance, vcpPort_t, port);
    port->buffering = false;
    usbVcpFlush(port);
}

static const struct serialPortVTable usbVTable[] = {
    {
        .serialWrite = usbVcpWrite,
        .serialTotalRxWaiting = usbVcpAvailable,
        .serialTotalTxFree = usbTxBytesFree,
        .serialRead = usbVcpRead,
        .serialSetBaudRate = usbVcpSetBaudRate,
        .isSerialTransmitBufferEmpty = isUsbVcpTransmitBufferEmpty,
        .setMode = usbVcpSetMode,
        .setCtrlLineStateCb = usbVcpSetCtrlLineStateCb,
        .setBaudRateCb = usbVcpSetBaudRateCb,
        .writeBuf = usbVcpWriteBuf,
        .beginWrite = usbVcpBeginWrite,
        .endWrite = usbVcpEndWrite
    }
};

void usbVcpInit(void)
{
    if (!ch32UsbHsPhyInit()) {
        return;
    }

    usb_rxsof_handler = usb_int_rxsof_handler;
    cdc_acm_init(0, 0);
#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
    usbCoreInitialized = true;
#endif

    NVIC_SetPriority(USBHS_IRQn, NVIC_PRIO_USB);
    NVIC_EnableIRQ(USBHS_IRQn);
}

#if defined(CH32H415) && defined(CH32H415_BOOT_SDI)
void ch32h415UsbMarkCoreInitialized(void)
{
    usbCoreInitialized = true;
}

bool ch32h415UsbIsReleasedForDebug(void)
{
    return usbReleasedForDebug;
}

void ch32h415UsbReleaseForDebug(void)
{
    /* Called from the system task, never the USB interrupt. Disable the IRQ
     * before taking down CherryUSB so no callback races the handoff. */
    NVIC_DisableIRQ(USBHS_IRQn);
    usbReleasedForDebug = true;
    usbCableDetectDeinit();
    if (usbCoreInitialized) {
        (void)usbd_deinitialize(0);
        usbCoreInitialized = false;
    }
    USBHSD->INT_EN = 0;
    /* Hold the link and SIE in reset, and clear pending USB state. H415
     * shares these pads with SDI; CONTROL=0 leaves SDI inaccessible after
     * a real USB initialization even when SW_CFG has been restored. */
    USBHSD->CONTROL = USBHS_UD_RST_LINK | USBHS_UD_RST_SIE | USBHS_UD_CLR_ALL;
    RCC_UTMIcmd(DISABLE);
    /* USBHS and SWCLK/SWDIO share PB8/PB9. Leave the USBHS peripheral clock
     * on: on H415, gating it also makes the WCH debug interface inaccessible.
     * The controller is held in reset above and the PHY is suspended. Do not
     * stop USBHS PLL: it may be the shared system clock source. */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_Disable, DISABLE);
    __asm volatile("fence iorw, iorw" ::: "memory");
}
#endif


serialPort_t *usbVcpOpen(void)
{
    vcpPort_t *s = &vcpPort;
    s->port.vTable = usbVTable;
    return &s->port;
}

uint32_t usbVcpGetBaudRate(serialPort_t *instance)
{
    UNUSED(instance);
    return cdc_vcp_line_coding.dwDTERate;
}
#endif
