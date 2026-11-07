/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "platform.h"

#ifdef USE_VCP

#include "build/atomic.h"
#include "common/utils.h"
#include "drivers/nvic.h"

#include "cdc_vcp_ch32h41x.h"
#include "usb_ch32h41x.h"

struct cdc_line_coding cdc_vcp_line_coding = {
    .dwDTERate = 115200,
    .bDataBits = 8,
    .bCharFormat = 0,
    .bParityType = 0,
};

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x02, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x04)
};

static const uint8_t config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x04)
};

static const uint8_t other_speed_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x04)
};

static const uint8_t other_speed_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x04)
};

static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x02, 0x02, 0x01, 0x01)
};

static char cdcSerialString[25];

static const char *const string_descriptors[] = {
    (const char[]){ 0x09, 0x04 }, /* Langid */
    "Betaflight",                 /* Manufacturer */
    USBD_PRODUCT_STRING,          /* Product */
    NULL,                         /* Serial Number (populated from chip UID) */
    "WCH HS Virtual COM Port",    /* Interface */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    UNUSED(speed);
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? config_descriptor_hs : config_descriptor_fs;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    UNUSED(speed);
    return device_quality_descriptor;
}

static const uint8_t *other_speed_descriptor_callback(uint8_t speed)
{
    return (speed == USB_SPEED_HIGH) ? other_speed_descriptor_hs : other_speed_descriptor_fs;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    UNUSED(speed);
    if (index >= ARRAYLEN(string_descriptors)) {
        return NULL;
    }
    if (index == USB_STRING_SERIAL_INDEX) {
        return ch32UsbGetUidSerialString(cdcSerialString);
    }
    return string_descriptors[index];
}

const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

#define RX_BUFFER_SIZE 2048
#define TX_BUFFER_SIZE 512

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_drivers_buffer[CDC_MAX_MPS];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t read_buffer[RX_BUFFER_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t write_buffer[TX_BUFFER_SIZE];

volatile uint8_t ep_tx_busy_flag = 0;
volatile uint8_t usbd_cdc_info = USBD_EVENT_UNKNOWN;

static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;
static volatile uint8_t cdc_configured = 0;
static volatile uint8_t cdc_connected = 0;
static volatile uint8_t rx_ep_paused = 0;

static uint16_t usbVcpOutMps(uint8_t busid)
{
    const uint16_t mps = usbd_get_ep_mps(busid, CDC_OUT_EP);
    return (mps != 0) ? mps : CDC_MAX_MPS;
}

#ifndef USB_VCP_OUT_MPS
#define USB_VCP_OUT_MPS(busid) (usbVcpOutMps(busid))
#endif

static uint16_t usbVcpRxUsedLocked(void)
{
    return (rx_head >= rx_tail)
        ? (uint16_t)(rx_head - rx_tail)
        : (uint16_t)(RX_BUFFER_SIZE - rx_tail + rx_head);
}

static uint16_t usbVcpRxFreeLocked(void)
{
    return (uint16_t)((RX_BUFFER_SIZE - 1U) - usbVcpRxUsedLocked());
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_INIT:
        cdc_configured = 0;
        cdc_connected = 0;
        usbd_cdc_info = (uint8_t)USBD_EVENT_UNKNOWN;
        ep_tx_busy_flag = 0;
        rx_head = 0;
        rx_tail = 0;
        rx_ep_paused = 0;
        usbVcpResetState();
        break;
    case USBD_EVENT_RESET:
        cdc_configured = 0;
        cdc_connected = 1;
        usbd_cdc_info = (uint8_t)USBD_EVENT_RESET;
        ep_tx_busy_flag = 0;
        rx_head = 0;
        rx_tail = 0;
        rx_ep_paused = 0;
        usbVcpResetState();
        break;
    case USBD_EVENT_CONNECTED:
        cdc_connected = 1;
        usbd_cdc_info = (uint8_t)USBD_EVENT_CONNECTED;
        break;
    case USBD_EVENT_DISCONNECTED:
    case USBD_EVENT_DEINIT:
        cdc_configured = 0;
        cdc_connected = 0;
        usbd_cdc_info = (uint8_t)USBD_EVENT_DISCONNECTED;
        ep_tx_busy_flag = 0;
        rx_head = 0;
        rx_tail = 0;
        rx_ep_paused = 0;
        usbVcpResetState();
        break;
    case USBD_EVENT_RESUME:
        if (cdc_connected) {
            usbd_cdc_info = cdc_configured ? (uint8_t)USBD_EVENT_CONFIGURED
                                           : (uint8_t)USBD_EVENT_RESUME;
        }
        break;
    case USBD_EVENT_SUSPEND:
        if (cdc_connected) {
            usbd_cdc_info = (uint8_t)USBD_EVENT_SUSPEND;
        }
        break;
    case USBD_EVENT_CONFIGURED:
        cdc_configured = 1;
        cdc_connected = 1;
        usbd_cdc_info = (uint8_t)USBD_EVENT_CONFIGURED;
        ep_tx_busy_flag = 0;
        rx_head = 0;
        rx_tail = 0;
        rx_ep_paused = 0;
        usbVcpResetState();
        /* Setup first OUT endpoint read transfer */
        usbd_ep_start_read(busid, CDC_OUT_EP, usb_drivers_buffer, USB_VCP_OUT_MPS(busid));
        break;
    case USBD_EVENT_SET_REMOTE_WAKEUP:
    case USBD_EVENT_CLR_REMOTE_WAKEUP:
    default:
        break;
    }
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    UNUSED(ep);
    for (uint32_t i = 0; i < nbytes; i++) {
        const uint16_t next_head = (uint16_t)((rx_head + 1U) % RX_BUFFER_SIZE);
        if (next_head == rx_tail) {
            break;
        }
        read_buffer[rx_head] = usb_drivers_buffer[i];
        rx_head = next_head;
    }

    const uint16_t out_mps = USB_VCP_OUT_MPS(busid);
    if (usbVcpRxFreeLocked() >= out_mps) {
        rx_ep_paused = 0;
        usbd_ep_start_read(busid, CDC_OUT_EP, usb_drivers_buffer, out_mps);
    } else {
        /* Leave Bulk-OUT in hardware NAK until FC drains enough RX buffer space */
        rx_ep_paused = 1;
    }
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    UNUSED(busid);
    UNUSED(ep);
    UNUSED(nbytes);
    ep_tx_busy_flag = 0;
    usbVcpTxDoneISR();
}

struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out,
};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in,
};

static struct usbd_interface intf0;

void cdc_acm_init(uint8_t busid, uintptr_t reg_base)
{
#ifdef CONFIG_USBDEV_ADVANCE_DESC
    usbd_desc_register(busid, &cdc_descriptor);
#endif
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf0));
    usbd_add_endpoint(busid, &cdc_out_ep);
    usbd_add_endpoint(busid, &cdc_in_ep);
    usbd_initialize(busid, reg_base, usbd_event_handler);
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;
    cdc_vcp_line_coding.bCharFormat = line_coding->bCharFormat;
    cdc_vcp_line_coding.bDataBits = line_coding->bDataBits;
    cdc_vcp_line_coding.bParityType = line_coding->bParityType;
    cdc_vcp_line_coding.dwDTERate = line_coding->dwDTERate;
    usbVcpBaudRateChanged(line_coding->dwDTERate);
}

void usbd_cdc_acm_set_control_line_state(uint8_t busid, uint8_t intf, uint16_t state)
{
    (void)busid;
    (void)intf;
    usbVcpControlLineStateChanged(state);
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;

    line_coding->dwDTERate = cdc_vcp_line_coding.dwDTERate;
    line_coding->bDataBits = cdc_vcp_line_coding.bDataBits;
    line_coding->bParityType = cdc_vcp_line_coding.bParityType;
    line_coding->bCharFormat = cdc_vcp_line_coding.bCharFormat;
}

uint16_t usb_vcp_rx_available(void)
{
    uint16_t count;
    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        count = usbVcpRxUsedLocked();
    }
    return count;
}

uint16_t usb_vcp_get_rx_data(uint8_t busid, uint8_t *buffer, uint32_t len)
{
    uint32_t count;

    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        count = usbVcpRxUsedLocked();
        if (count > len) {
            count = len;
        }
        for (uint32_t i = 0; i < count; i++) {
            buffer[i] = read_buffer[rx_tail];
            rx_tail++;
            if (rx_tail >= RX_BUFFER_SIZE) {
                rx_tail = 0;
            }
        }
        if (rx_ep_paused && cdc_configured) {
            const uint16_t out_mps = USB_VCP_OUT_MPS(busid);
            if (usbVcpRxFreeLocked() >= out_mps) {
                if (usbd_ep_start_read(busid, CDC_OUT_EP, usb_drivers_buffer, out_mps) == 0) {
                    rx_ep_paused = 0;
                }
            }
        }
    }
    return (uint16_t)count;
}

uint16_t usb_vcp_send_data(uint8_t busid, uint8_t *buffer, uint16_t len)
{
    uint16_t ret = 1;

    if (len > TX_BUFFER_SIZE) {
        return 1;
    }

    ATOMIC_BLOCK(NVIC_PRIO_USB) {
        if (ep_tx_busy_flag == 0) {
            ep_tx_busy_flag = 1;
            if (len > 0) {
                memcpy(write_buffer, buffer, len);
            }
            if (usbd_ep_start_write(busid, CDC_IN_EP, write_buffer, len) < 0) {
                ep_tx_busy_flag = 0;
            } else {
                ret = 0;
            }
        }
    }

    return ret;
}

#endif /* USE_VCP */
