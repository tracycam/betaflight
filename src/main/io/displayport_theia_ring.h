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
#define THEIA_COLS 30u
#define THEIA_ROWS 16u
/* BF's own clock measures both deadlines; no cross-core clock assumption. */
#define THEIA_HEALTH_INTERVAL_US 250000u
#define THEIA_HEALTH_TIMEOUT_US 1500000u

typedef struct {
    volatile uint8_t *ring;
    volatile uint32_t *writePos, *readPos, *bootState, *hwReady;
    volatile uint32_t *abiVersion, *resync, *videoFormat, *fontDoneSeq;
} theiaMailbox_t;
typedef struct {
    theiaMailbox_t mailbox;
    uint16_t cells[THEIA_ROWS * THEIA_COLS];
    uint16_t committed[THEIA_ROWS * THEIA_COLS];
    uint32_t seenResync, fontSeq, fontResync;
    bool dirty, staged, released, wasReady, fontOutstanding;
    uint32_t healthTarget, healthResync, healthLastAck;
    bool healthPending, healthOnline;
} theiaProducer_t;
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
bool theiaProducerFontWrite(theiaProducer_t *p, uint16_t glyph, const uint8_t bits[54]);
typedef enum { THEIA_FONT_PENDING, THEIA_FONT_DONE, THEIA_FONT_ERROR } theiaFontStatus_e;
theiaFontStatus_e theiaProducerFontStatus(theiaProducer_t *p);
void theiaProducerFontCancel(theiaProducer_t *p);
uint32_t theiaProducerBytesFree(const theiaProducer_t *p);
