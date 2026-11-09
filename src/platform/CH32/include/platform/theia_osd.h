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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "io/displayport_theia_ring.h"
#include "platform/h415_contract.h"

_Static_assert(THEIA_RING_SIZE == H415C_SHRAM_DP_RING_SIZE, "Theia ring size mismatch");
_Static_assert(THEIA_ABI_VERSION == H415C_ABI_VERSION, "Theia ABI version mismatch");
_Static_assert(THEIA_HW_READY_MAGIC == H415C_HW_READY_MAGIC, "Theia HW_READY magic mismatch");
_Static_assert(THEIA_HW_OSD_ENABLED == H415C_HW_OSD_ENABLED, "Theia HW_OSD_ENABLED mismatch");
_Static_assert(THEIA_BS_OSD_RUNNING == H415C_BS_V3_OSD_RUNNING, "Theia OSD running state mismatch");
_Static_assert(THEIA_VIDEO_PAL == H415C_VIDEO_PAL, "Theia PAL constant mismatch");
_Static_assert(THEIA_VIDEO_NTSC == H415C_VIDEO_NTSC, "Theia NTSC constant mismatch");
_Static_assert(THEIA_FONT_GLYPHS == H415C_FONT_GLYPHS, "Theia font glyph count mismatch");
_Static_assert(THEIA_FONT_GLYPH_BYTES == H415C_FONT_BYTES_PER_GLYPH, "Theia font glyph size mismatch");

static inline bool theiaPlatformBindMailbox(theiaMailbox_t *mailbox)
{
    if (!mailbox) {
        return false;
    }

    *mailbox = (theiaMailbox_t){
        .ring = (volatile uint8_t *)(uintptr_t)H415C_SHRAM_DP_RING,
        .writePos = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_DP_WRITE_POS,
        .readPos = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_DP_READ_POS,
        .bootState = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_BOOT_STATE,
        .hwReady = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_HW_READY,
        .abiVersion = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_ABI_VERSION,
        .resync = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_DP_RESYNC,
        .videoFormat = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_VIDEO_FORMAT,
        .fontDoneSeq = (volatile uint32_t *)(uintptr_t)H415C_SHRAM_FONT_DONE_SEQ,
    };

    return true;
}
