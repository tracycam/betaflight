/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "platform.h"

#if defined(USE_VCP) || defined(USE_USB_MSC)

#include "usb_ch32h41x_usbhs_reg.h"
#include "usbd_core.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

#define USB_BASE 0x40030000u
#define CH32H41x_USBHS_DEV ((USBHSD_TypeDef *)USB_BASE)

#ifndef USB_MEM_ALIGNX
#define USB_MEM_ALIGNX __attribute__((aligned(4)))
#endif

#define USB_SET_RX_DMA(ep_idx, addr) \
    (*(volatile uint32_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP1_RX_DMA) + 4U * ((ep_idx) - 1U)) = (addr))
#define USB_SET_TX_DMA(ep_idx, addr) \
    (*(volatile uint32_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP1_TX_DMA) + 4U * ((ep_idx) - 1U)) = (addr))

#define USB_SET_MAX_LEN(ep_idx, len) \
    (*(volatile uint16_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_MAX_LEN) + 4U * (ep_idx)) = (len))
#define USB_SET_TX_LEN(ep_idx, len) \
    (*(volatile uint16_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_TX_LEN) + 4U * (ep_idx)) = (len))
#define USB_GET_TX_LEN(ep_idx) \
    (*(volatile uint16_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_TX_LEN) + 4U * (ep_idx)))
#define USB_SET_TX_CTRL(ep_idx, val) \
    (*(volatile uint8_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_TX_CTRL) + 4U * (ep_idx)) = (val))
#define USB_GET_TX_CTRL(ep_idx) \
    (*(volatile uint8_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_TX_CTRL) + 4U * (ep_idx)))
#define USB_SET_RX_CTRL(ep_idx, val) \
    (*(volatile uint8_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_RX_CTRL) + 4U * (ep_idx)) = (val))
#define USB_GET_RX_CTRL(ep_idx) \
    (*(volatile uint8_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_RX_CTRL) + 4U * (ep_idx)))
#define EPn_SET_TX_NAK(ep_idx) \
    USB_SET_TX_CTRL((ep_idx), (uint8_t)((USB_GET_TX_CTRL(ep_idx) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK))
#define EPn_SET_TX_VALID(ep_idx) \
    USB_SET_TX_CTRL((ep_idx), (uint8_t)((USB_GET_TX_CTRL(ep_idx) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK))
#define EPn_SET_RX_NAK(ep_idx) \
    USB_SET_RX_CTRL((ep_idx), (uint8_t)((USB_GET_RX_CTRL(ep_idx) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_NAK))
#define EPn_SET_RX_VALID(ep_idx) \
    USB_SET_RX_CTRL((ep_idx), (uint8_t)((USB_GET_RX_CTRL(ep_idx) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK))
#define EPn_GET_RX_LEN(ep_idx) \
    (*(volatile uint16_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->USB_EP0_RX_LEN) + 4U * (ep_idx)))
#define EPn_SET_TX_LEN(ep_idx, len) \
    (*(volatile uint16_t *)((uintptr_t)(&CH32H41x_USBHS_DEV->UEP0_TX_LEN) + 4U * (ep_idx)) = (len))
#define EPn_CLEAR_TX_DONE(ep_idx) \
    USB_SET_TX_CTRL((ep_idx), (uint8_t)(USB_GET_TX_CTRL(ep_idx) & ~USBHS_UEP_T_DONE))
#define EPn_CLEAR_RX_DONE(ep_idx) \
    USB_SET_RX_CTRL((ep_idx), (uint8_t)(USB_GET_RX_CTRL(ep_idx) & ~USBHS_UEP_R_DONE))
#define EPn_SET_TX_ISO_VALID(ep_idx) ((void)(ep_idx))
#define EPn_SET_RX_ISO_VALID(ep_idx) ((void)(ep_idx))

#ifdef __riscv
#define USB_MEMORY_BARRIER() __asm__ volatile ("fence iorw, iorw" ::: "memory")
#else
#define USB_MEMORY_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

#ifndef CONFIG_USBDEV_EP_NUM
#define CONFIG_USBDEV_EP_NUM 8
#endif

typedef struct _usbd_ep_info {
    uint16_t mps;
    uint8_t eptype;
    uint8_t ep_enable;
    uint8_t *xfer_buf;
    uint32_t xfer_len;
    uint32_t actual_xfer_len;
} usbd_ep_info;

static struct _ch32h41x_hs_core_prvi {
    uint8_t address;
    usbd_ep_info ep_in[CONFIG_USBDEV_EP_NUM];
    usbd_ep_info ep_out[CONFIG_USBDEV_EP_NUM];
    USB_MEM_ALIGNX struct usb_setup_packet setup;
} usb_dc_cfg;

__WEAK void usb_dc_low_level_init(void)
{
}

__WEAK void usb_dc_low_level_deinit(void)
{
}

usb_rxsof_handler_t usb_rxsof_handler = NULL;

int usb_dc_init(uint8_t busid)
{
    UNUSED(busid);

    usb_dc_low_level_init();
    CH32H41x_USBHS_DEV->CONTROL = USBHS_UD_RST_LINK | USBHS_UD_PHY_SUSPENDM;
    uint8_t int_en = USBHS_UDIE_BUS_RST | USBHS_UDIE_SUSPEND |
                     USBHS_UDIE_BUS_SLEEP | USBHS_UDIE_LPM_ACT |
                     USBHS_UDIE_TRANSFER | USBHS_UDIE_LINK_RDY;
    if (usb_rxsof_handler != NULL) {
        int_en |= USBHS_UDIE_SOF_ACT;
    }
    CH32H41x_USBHS_DEV->INT_EN = int_en;
    CH32H41x_USBHS_DEV->UEP_TX_EN = 0xffff;
    CH32H41x_USBHS_DEV->UEP_RX_EN = 0xffff;
#ifdef CONFIG_USB_HS
    CH32H41x_USBHS_DEV->BASE_MODE = USBHS_UD_SPEED_HIGH;
#else
    CH32H41x_USBHS_DEV->BASE_MODE = USBHS_UD_SPEED_FULL;
#endif
    CH32H41x_USBHS_DEV->CONTROL = USBHS_UD_DEV_EN | USBHS_UD_DMA_EN |
                                  USBHS_UD_LPM_EN | USBHS_UD_PHY_SUSPENDM;

    CH32H41x_USBHS_DEV->UEP_T_TOG_AUTO = 0xfe;
    CH32H41x_USBHS_DEV->UEP_R_TOG_AUTO = 0xfe;

    return 0;
}

int usb_dc_deinit(uint8_t busid)
{
    UNUSED(busid);

    CH32H41x_USBHS_DEV->CONTROL |= USBHS_UD_RST_SIE;
    CH32H41x_USBHS_DEV->CONTROL &= (uint8_t)~USBHS_UD_RST_SIE;
    usb_dc_low_level_deinit();
    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t address)
{
    UNUSED(busid);

    if (address == 0) {
        CH32H41x_USBHS_DEV->DEV_AD = (uint8_t)((CH32H41x_USBHS_DEV->DEV_AD & 0x80U) | address);
    }
    usb_dc_cfg.address = address;
    return 0;
}

int usbd_set_remote_wakeup(uint8_t busid)
{
    UNUSED(busid);
    return -1;
}

uint8_t usbd_get_port_speed(uint8_t busid)
{
    UNUSED(busid);
#ifdef CONFIG_USB_HS
    return (CH32H41x_USBHS_DEV->MIS_ST & USBHS_UDMS_HS_MOD) ? USB_SPEED_HIGH : USB_SPEED_FULL;
#else
    return USB_SPEED_FULL;
#endif
}

int usbd_ep_open(uint8_t busid, const struct usb_endpoint_descriptor *ep)
{
    UNUSED(busid);

    uint8_t epid = USB_EP_GET_IDX(ep->bEndpointAddress);
    if (epid >= CONFIG_USBDEV_EP_NUM) {
        USB_LOG_ERR("Ep addr %02x overflow\r\n", ep->bEndpointAddress);
        return -1;
    }

    uint16_t mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
    USB_SET_MAX_LEN(epid, mps);

    if (USB_EP_DIR_IS_IN(ep->bEndpointAddress)) {
        usb_dc_cfg.ep_in[epid].ep_enable = true;
        usb_dc_cfg.ep_in[epid].mps = mps;
        usb_dc_cfg.ep_in[epid].eptype = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
        USB_SET_TX_CTRL(epid, USBHS_UEP_T_RES_NAK);
        EPn_CLEAR_TX_DONE(epid);
    } else if (USB_EP_DIR_IS_OUT(ep->bEndpointAddress)) {
        usb_dc_cfg.ep_out[epid].ep_enable = true;
        usb_dc_cfg.ep_out[epid].mps = mps;
        usb_dc_cfg.ep_out[epid].eptype = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
        USB_SET_RX_CTRL(epid, USBHS_UEP_R_RES_NAK);
        EPn_CLEAR_RX_DONE(epid);
    }

    return 0;
}

int usbd_ep_close(uint8_t busid, const uint8_t ep)
{
    UNUSED(busid);

    uint8_t epid = USB_EP_GET_IDX(ep);
    if (epid >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (USB_EP_DIR_IS_IN(ep)) {
        usb_dc_cfg.ep_in[epid].ep_enable = false;
    } else if (USB_EP_DIR_IS_OUT(ep)) {
        usb_dc_cfg.ep_out[epid].ep_enable = false;
    }
    return 0;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t ep)
{
    UNUSED(busid);

    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    if (ep_idx >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (ep_idx == 0) {
        /* Protocol stall on EP0: keep UEP0_DMA pointing at setup buffer for the next SETUP token */
        CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
        USB_MEMORY_BARRIER();
        CH32H41x_USBHS_DEV->UEP0_TX_CTRL = USBHS_UEP_T_RES_STALL;
        CH32H41x_USBHS_DEV->UEP0_RX_CTRL = USBHS_UEP_R_RES_STALL;
    } else if (USB_EP_DIR_IS_OUT(ep)) {
        USB_SET_RX_CTRL(ep_idx, (uint8_t)((USB_GET_RX_CTRL(ep_idx) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_STALL));
    } else {
        USB_SET_TX_CTRL(ep_idx, (uint8_t)((USB_GET_TX_CTRL(ep_idx) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_STALL));
    }
    return 0;
}

int usbd_ep_clear_stall(uint8_t busid, const uint8_t ep)
{
    UNUSED(busid);

    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    if (ep_idx >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (USB_EP_DIR_IS_OUT(ep)) {
        if (ep_idx == 0) {
            CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
            USB_MEMORY_BARRIER();
            USB_SET_RX_CTRL(0, USBHS_UEP_R_RES_ACK | USBHS_UEP_R_TOG_DATA0);
        } else {
            /* Reset toggle to DATA0 and hold in NAK until class driver arms RX DMA */
            USB_SET_RX_CTRL(ep_idx, USBHS_UEP_R_RES_NAK | USBHS_UEP_R_TOG_DATA0);
        }
    } else {
        USB_SET_TX_CTRL(ep_idx, USBHS_UEP_T_RES_NAK | USBHS_UEP_T_TOG_DATA0);
    }
    return 0;
}

int usbd_ep_is_stalled(uint8_t busid, const uint8_t ep, uint8_t *stalled)
{
    UNUSED(busid);

    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    if (!stalled || ep_idx >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (USB_EP_DIR_IS_OUT(ep)) {
        *stalled = ((USB_GET_RX_CTRL(ep_idx) & USBHS_UEP_R_RES_MASK) == USBHS_UEP_R_RES_STALL) ? 1U : 0U;
    } else {
        *stalled = ((USB_GET_TX_CTRL(ep_idx) & USBHS_UEP_T_RES_MASK) == USBHS_UEP_T_RES_STALL) ? 1U : 0U;
    }
    return 0;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t ep, const uint8_t *data, uint32_t data_len)
{
    UNUSED(busid);

    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    if (ep_idx >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }
    if (!data && data_len) {
        return -1;
    }
    if (!usb_dc_cfg.ep_in[ep_idx].ep_enable) {
        return -2;
    }
    if ((uintptr_t)data & 0x03U) {
        return -3;
    }

    usb_dc_cfg.ep_in[ep_idx].xfer_buf = (uint8_t *)data;
    usb_dc_cfg.ep_in[ep_idx].xfer_len = data_len;
    usb_dc_cfg.ep_in[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        if (data_len == 0) {
            USB_SET_TX_LEN(0, 0);
        } else {
            data_len = MIN(data_len, usb_dc_cfg.ep_in[0].mps);
            USB_SET_TX_LEN(0, (uint16_t)data_len);
            CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)data;
        }
    } else {
        if (data_len == 0) {
            USB_SET_TX_LEN(ep_idx, 0);
        } else {
            data_len = MIN(data_len, usb_dc_cfg.ep_in[ep_idx].mps);
            USB_SET_TX_LEN(ep_idx, (uint16_t)data_len);
            USB_SET_TX_DMA(ep_idx, (uint32_t)(uintptr_t)data);
        }
    }

    USB_MEMORY_BARRIER();
    EPn_SET_TX_VALID(ep_idx);
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t *data, uint32_t data_len)
{
    UNUSED(busid);

    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    if (ep_idx >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }
    if (!data && data_len) {
        return -1;
    }
    if (!usb_dc_cfg.ep_out[ep_idx].ep_enable) {
        return -2;
    }
    if ((uintptr_t)data & 0x03U) {
        return -3;
    }

    usb_dc_cfg.ep_out[ep_idx].xfer_buf = (uint8_t *)data;
    usb_dc_cfg.ep_out[ep_idx].xfer_len = data_len;
    usb_dc_cfg.ep_out[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        if (data_len == 0) {
            CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
        } else {
            CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)data;
        }
    } else {
        USB_SET_RX_DMA(ep_idx, (uint32_t)(uintptr_t)data);
    }

    USB_MEMORY_BARRIER();
    EPn_SET_RX_VALID(ep_idx);
    return 0;
}

static inline void handle_ep0_in(void)
{
    switch (usb_dc_cfg.setup.bmRequestType >> USB_REQUEST_DIR_SHIFT) {
    case 1:
        CH32H41x_USBHS_DEV->UEP0_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
        EPn_SET_TX_NAK(0);
        if (usb_dc_cfg.ep_in[0].xfer_len > usb_dc_cfg.ep_in[0].mps) {
            usb_dc_cfg.ep_in[0].xfer_len -= usb_dc_cfg.ep_in[0].mps;
            usb_dc_cfg.ep_in[0].actual_xfer_len += usb_dc_cfg.ep_in[0].mps;
            usbd_event_ep_in_complete_handler(0, 0x80, usb_dc_cfg.ep_in[0].actual_xfer_len);
        } else {
            usb_dc_cfg.ep_in[0].actual_xfer_len += usb_dc_cfg.ep_in[0].xfer_len;
            usb_dc_cfg.ep_in[0].xfer_len = 0;
            usbd_event_ep_in_complete_handler(0, 0x80, usb_dc_cfg.ep_in[0].actual_xfer_len);
        }
        break;
    case 0:
        if (usb_dc_cfg.setup.bRequest == USB_REQUEST_SET_ADDRESS) {
            CH32H41x_USBHS_DEV->DEV_AD = usb_dc_cfg.address;
        }
        CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
        EPn_SET_TX_NAK(0);
        USB_MEMORY_BARRIER();
        EPn_SET_RX_VALID(0);
        break;
    default:
        break;
    }
}

static inline void handle_non_ep0_in(uint8_t epid)
{
    EPn_SET_TX_NAK(epid);
    if (usb_dc_cfg.ep_in[epid].xfer_len > usb_dc_cfg.ep_in[epid].mps) {
        usb_dc_cfg.ep_in[epid].xfer_buf += usb_dc_cfg.ep_in[epid].mps;
        usb_dc_cfg.ep_in[epid].xfer_len -= usb_dc_cfg.ep_in[epid].mps;
        usb_dc_cfg.ep_in[epid].actual_xfer_len += usb_dc_cfg.ep_in[epid].mps;

        uint32_t write_count = MIN(usb_dc_cfg.ep_in[epid].xfer_len, usb_dc_cfg.ep_in[epid].mps);
        USB_SET_TX_LEN(epid, (uint16_t)write_count);
        USB_SET_TX_DMA(epid, (uint32_t)(uintptr_t)usb_dc_cfg.ep_in[epid].xfer_buf);

        if (usb_dc_cfg.ep_in[epid].eptype != USB_ENDPOINT_TYPE_ISOCHRONOUS) {
            USB_MEMORY_BARRIER();
            EPn_SET_TX_VALID(epid);
        } else {
            EPn_SET_TX_ISO_VALID(epid);
        }
    } else {
        usb_dc_cfg.ep_in[epid].actual_xfer_len += usb_dc_cfg.ep_in[epid].xfer_len;
        usb_dc_cfg.ep_in[epid].xfer_len = 0;
        usbd_event_ep_in_complete_handler(0, (uint8_t)(epid | 0x80U), usb_dc_cfg.ep_in[epid].actual_xfer_len);
    }
}

static inline void usb_process_ep_in(uint8_t epid)
{
    EPn_SET_TX_NAK(epid);
    EPn_CLEAR_TX_DONE(epid);
    USB_MEMORY_BARRIER();

    if (epid == 0) {
        handle_ep0_in();
    } else {
        handle_non_ep0_in(epid);
    }
}

static inline void usb_process_ep_out(uint8_t epid)
{
    EPn_SET_RX_NAK(epid);
    USB_MEMORY_BARRIER();
    const uint8_t rx_ctrl = USB_GET_RX_CTRL(epid);
    const uint32_t read_count = EPn_GET_RX_LEN(epid);
    EPn_CLEAR_RX_DONE(epid);
    USB_MEMORY_BARRIER();

    if (epid == 0) {
        if (rx_ctrl & USBHS_UEP_R_SETUP_IS) {
            CH32H41x_USBHS_DEV->UEP0_RX_CTRL |= USBHS_UEP_R_TOG_DATA1;
            CH32H41x_USBHS_DEV->UEP0_TX_CTRL |= USBHS_UEP_T_TOG_DATA1;
            usbd_event_ep0_setup_complete_handler(0, (uint8_t *)&usb_dc_cfg.setup);
        } else {
            CH32H41x_USBHS_DEV->UEP0_RX_CTRL ^= USBHS_UEP_R_TOG_DATA1;
            usb_dc_cfg.ep_out[0].actual_xfer_len += read_count;
            usb_dc_cfg.ep_out[0].xfer_len = (usb_dc_cfg.ep_out[0].xfer_len > read_count)
                ? (usb_dc_cfg.ep_out[0].xfer_len - read_count)
                : 0;
            usbd_event_ep_out_complete_handler(0, 0x00, usb_dc_cfg.ep_out[0].actual_xfer_len);
            if (read_count == 0) {
                CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
                USB_MEMORY_BARRIER();
                EPn_SET_RX_VALID(0);
            }
        }
    } else {
        if (rx_ctrl & USBHS_UEP_R_TOG_MATCH) {
            usb_dc_cfg.ep_out[epid].xfer_buf += read_count;
            usb_dc_cfg.ep_out[epid].actual_xfer_len += read_count;
            usb_dc_cfg.ep_out[epid].xfer_len = (usb_dc_cfg.ep_out[epid].xfer_len > read_count)
                ? (usb_dc_cfg.ep_out[epid].xfer_len - read_count)
                : 0;

            if ((read_count < usb_dc_cfg.ep_out[epid].mps) || (usb_dc_cfg.ep_out[epid].xfer_len == 0)) {
                usbd_event_ep_out_complete_handler(0, (uint8_t)(epid & 0x7fU), usb_dc_cfg.ep_out[epid].actual_xfer_len);
            } else {
                USB_SET_RX_DMA(epid, (uint32_t)(uintptr_t)usb_dc_cfg.ep_out[epid].xfer_buf);
                if (usb_dc_cfg.ep_out[epid].eptype != USB_ENDPOINT_TYPE_ISOCHRONOUS) {
                    USB_MEMORY_BARRIER();
                    EPn_SET_RX_VALID(epid);
                } else {
                    EPn_SET_RX_ISO_VALID(epid);
                }
            }
        } else if (usb_dc_cfg.ep_out[epid].ep_enable && usb_dc_cfg.ep_out[epid].xfer_buf != NULL) {
            /* Duplicate/retransmitted OUT packet with mismatched DATA toggle: discard and re-arm same buffer */
            USB_SET_RX_DMA(epid, (uint32_t)(uintptr_t)usb_dc_cfg.ep_out[epid].xfer_buf);
            if (usb_dc_cfg.ep_out[epid].eptype != USB_ENDPOINT_TYPE_ISOCHRONOUS) {
                USB_MEMORY_BARRIER();
                EPn_SET_RX_VALID(epid);
            } else {
                EPn_SET_RX_ISO_VALID(epid);
            }
        }
    }
}

static inline void usb_trans_end_process(void)
{
    uint8_t epid = (uint8_t)(CH32H41x_USBHS_DEV->INT_ST & USBHS_UDIS_EP_ID_MASK);
    if (CH32H41x_USBHS_DEV->INT_ST & USBHS_UDIS_EP_DIR) {
        usb_process_ep_in(epid);
    } else {
        usb_process_ep_out(epid);
    }
}

void USBD_IRQHandler(uint8_t busid)
{
    UNUSED(busid);
    const uint8_t intflag = CH32H41x_USBHS_DEV->INT_FG;

    if (intflag & USBHS_UDIF_TRANSFER) {
        usb_trans_end_process();
    } else if (intflag & USBHS_UDIF_BUS_RST) {
        CH32H41x_USBHS_DEV->DEV_AD = 0;
        usbd_event_reset_handler(0);
        CH32H41x_USBHS_DEV->UEP0_DMA = (uint32_t)(uintptr_t)&usb_dc_cfg.setup;
        USB_MEMORY_BARRIER();
        CH32H41x_USBHS_DEV->UEP0_TX_CTRL = USBHS_UEP_T_RES_NAK;
        CH32H41x_USBHS_DEV->UEP0_RX_CTRL = USBHS_UEP_R_RES_ACK;
        CH32H41x_USBHS_DEV->INT_FG = USBHS_UDIF_BUS_RST;
    } else if (intflag & USBHS_UDIF_SUSPEND) {
        CH32H41x_USBHS_DEV->INT_FG = USBHS_UDIF_SUSPEND;
        if (CH32H41x_USBHS_DEV->MIS_ST & USBHS_UDMS_SUSPEND) {
            usbd_event_suspend_handler(0);
        } else {
            usbd_event_resume_handler(0);
        }
    } else if (intflag & USBHS_UDIF_RX_SOF) {
        if (usb_rxsof_handler != NULL) {
            usb_rxsof_handler();
        }
        CH32H41x_USBHS_DEV->INT_FG = USBHS_UDIF_RX_SOF;
    } else {
        CH32H41x_USBHS_DEV->INT_FG = intflag;
    }
}

void USBHS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBHS_IRQHandler(void)
{
    USBD_IRQHandler(0);
}

#endif /* defined(USE_VCP) || defined(USE_USB_MSC) */
