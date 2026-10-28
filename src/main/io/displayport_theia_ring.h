/* TheiaOSD SPSC producer. Pure C so the exact firmware producer can be
 * exercised against the OSD consumer in host tests. GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define THEIA_COLS 30u
#define THEIA_ROWS 16u

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
} theiaProducer_t;
void theiaProducerInit(theiaProducer_t *p, const theiaMailbox_t *mailbox);
bool theiaProducerReady(const theiaProducer_t *p);
void theiaProducerClear(theiaProducer_t *p);
void theiaProducerWrite(theiaProducer_t *p, unsigned col, unsigned row,
                        uint8_t attr, const uint8_t *text, unsigned length);
bool theiaProducerDraw(theiaProducer_t *p);
bool theiaProducerHeartbeat(theiaProducer_t *p);
bool theiaProducerRelease(theiaProducer_t *p);
bool theiaProducerFontWrite(theiaProducer_t *p, uint16_t glyph, const uint8_t bits[54]);
typedef enum { THEIA_FONT_PENDING, THEIA_FONT_DONE, THEIA_FONT_ERROR } theiaFontStatus_e;
theiaFontStatus_e theiaProducerFontStatus(theiaProducer_t *p);
void theiaProducerFontCancel(theiaProducer_t *p);
uint32_t theiaProducerBytesFree(const theiaProducer_t *p);
