/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Betaflight. If not, see <http://www.gnu.org/licenses/>.
 */

/* TheiaOSD SPSC producer. Pure C so the exact firmware producer can be
 * exercised against the OSD consumer in host tests. GPL-3.0-or-later. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define THEIA_COLS                30u
#define THEIA_ROWS                16u
#define THEIA_RING_SIZE           (16u * 1024u)
#define THEIA_ABI_VERSION         4u
#define THEIA_HW_READY_MAGIC      0x48574400u
#define THEIA_HW_OSD_ENABLED      1u
#define THEIA_BS_OSD_RUNNING      0xA11C0002u
#define THEIA_VIDEO_PAL           1u
#define THEIA_VIDEO_NTSC          2u
#define THEIA_FONT_GLYPHS         512u
#define THEIA_FONT_GLYPH_BYTES    54u

/* BF's own clock measures both deadlines; no cross-core clock assumption. */
#define THEIA_HEALTH_INTERVAL_US  250000u
#define THEIA_HEALTH_TIMEOUT_US   1500000u

typedef struct theiaMailbox_s {
    volatile uint8_t *ring;
    volatile uint32_t *writePos;
    volatile uint32_t *readPos;
    volatile uint32_t *bootState;
    volatile uint32_t *hwReady;
    volatile uint32_t *abiVersion;
    volatile uint32_t *resync;
    volatile uint32_t *videoFormat;
    volatile uint32_t *fontDoneSeq;
} theiaMailbox_t;

typedef struct theiaProducer_s {
    theiaMailbox_t mailbox;
    uint16_t cells[THEIA_ROWS * THEIA_COLS];
    uint16_t committed[THEIA_ROWS * THEIA_COLS];
    uint32_t seenResync;
    uint32_t fontSeq;
    uint32_t fontResync;
    bool dirty;
    bool staged;
    bool released;
    bool wasReady;
    bool fontOutstanding;
    uint32_t healthTarget;
    uint32_t healthResync;
    uint32_t healthLastAck;
    bool healthPending;
    bool healthOnline;
} theiaProducer_t;

typedef enum {
    THEIA_FONT_PENDING,
    THEIA_FONT_DONE,
    THEIA_FONT_ERROR,
} theiaFontStatus_e;

void theiaProducerInit(theiaProducer_t *p, const theiaMailbox_t *mailbox);
bool theiaProducerReady(const theiaProducer_t *p);
void theiaProducerClear(theiaProducer_t *p);
void theiaProducerWrite(theiaProducer_t *p, unsigned col, unsigned row,
                        uint8_t attr, const uint8_t *text, unsigned length);
void theiaProducerCommit(theiaProducer_t *p);
bool theiaProducerPollHealth(theiaProducer_t *p, uint32_t nowUs);
bool theiaProducerDraw(theiaProducer_t *p);
bool theiaProducerHeartbeat(theiaProducer_t *p);
bool theiaProducerRelease(theiaProducer_t *p);
bool theiaProducerFontWrite(theiaProducer_t *p, uint16_t glyph, const uint8_t bits[THEIA_FONT_GLYPH_BYTES]);
theiaFontStatus_e theiaProducerFontStatus(theiaProducer_t *p);
void theiaProducerFontCancel(theiaProducer_t *p);
uint32_t theiaProducerBytesFree(const theiaProducer_t *p);
