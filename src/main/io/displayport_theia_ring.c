/* TheiaOSD ABI v4 producer: no HSEM, waits, allocation or serial output.
 * A screen is reserved and published as one CLEAR/WRITE/DRAW batch. */
#include "platform.h"
#include "io/displayport_theia_ring.h"
#include "io/h415_contract.h"
#include <string.h>

#define MASK (H415C_SHRAM_DP_RING_SIZE - 1u)
static void acquire(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}
static void releaseFence(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static unsigned span(unsigned payload)
{
    return (payload + 11u) & ~3u;
}

bool theiaProducerReady(const theiaProducer_t *p)
{
    const theiaMailbox_t *m = &p->mailbox;
    return *m->abiVersion == H415C_ABI_VERSION &&
           *m->hwReady == (H415C_HW_READY_MAGIC | H415C_HW_OSD_ENABLED) &&
           *m->bootState == H415C_BS_V3_OSD_RUNNING;
}
uint32_t theiaProducerBytesFree(const theiaProducer_t *p)
{
    if (!theiaProducerReady(p)) {
        return 0;
    }
    uint32_t write = *p->mailbox.writePos, read = *p->mailbox.readPos;
    acquire();
    uint32_t used = write - read;
    if (((write | read) & 3u) || used > H415C_SHRAM_DP_RING_SIZE) {
        return 0;
    }
    return H415C_SHRAM_DP_RING_SIZE - used;
}
static bool reserve(theiaProducer_t *p, unsigned bytes, uint32_t *position)
{
    if (theiaProducerBytesFree(p) < bytes) {
        return false;
    }
    *position = *p->mailbox.writePos;
    return true;
}
static void byte(theiaProducer_t *p, uint32_t *position, uint8_t value)
{
    p->mailbox.ring[(*position)++ & MASK] = value;
}
static void record(theiaProducer_t *p, uint32_t *position, const uint8_t *payload, unsigned length)
{
    const uint32_t end = *position + span(length);
    const unsigned frameLength = length + 6u;
    byte(p, position, (uint8_t)(frameLength >> 8));
    byte(p, position, (uint8_t)frameLength);
    byte(p, position, '$'); byte(p, position, 'M'); byte(p, position, '>');
    byte(p, position, (uint8_t)length); byte(p, position, 0xb6);
    uint8_t crc = (uint8_t)(length ^ 0xb6u);
    for (unsigned i = 0; i < length; i++) {
        byte(p, position, payload[i]); crc ^= payload[i];
    }
    byte(p, position, crc);
    while (*position != end) {
        byte(p, position, 0);
    }
}
static void publish(theiaProducer_t *p, uint32_t position)
{
    releaseFence();
    *p->mailbox.writePos = position;
}
static bool command(theiaProducer_t *p, uint8_t subcommand)
{
    uint32_t position;
    if (!reserve(p, span(1), &position)) {
        return false;
    }
    record(p, &position, &subcommand, 1);
    publish(p, position);
    return true;
}
void theiaProducerInit(theiaProducer_t *p, const theiaMailbox_t *mailbox)
{
    memset(p, 0, sizeof(*p)); p->mailbox = *mailbox; p->dirty = true;
    /* BL owns epoch initialization; never reset another core's cursor. */
    p->seenResync = *mailbox->resync;
    p->fontSeq = *mailbox->fontDoneSeq;
}
void theiaProducerClear(theiaProducer_t *p)
{
    memset(p->cells, 0, sizeof(p->cells)); p->staged = true;
}
void theiaProducerWrite(theiaProducer_t *p, unsigned col, unsigned row,
                        uint8_t attr, const uint8_t *text, unsigned length)
{
    if (col >= THEIA_COLS || row >= THEIA_ROWS) {
        return;
    }
    if (length > THEIA_COLS - col) {
        length = THEIA_COLS - col;
    }
    for (unsigned i = 0; i < length; i++) {
        uint16_t cell = text[i] | ((uint16_t)(attr & 0x60u) << 8);
        if (p->cells[row * THEIA_COLS + col + i] != cell) {
            p->cells[row * THEIA_COLS + col + i] = cell; p->staged = true;
        }
    }
}
/* Same runs are counted and encoded; the shadow cannot change because
 * every producer entry point is called in the BF cooperative task context. */
static unsigned screen(theiaProducer_t *p, uint32_t *position)
{
    unsigned bytes = 2u * span(1);
    uint8_t payload[4u + THEIA_COLS];
    if (position) {
        payload[0] = 2; record(p, position, payload, 1);
    }
    for (unsigned row = 0; row < THEIA_ROWS; row++) {
        for (unsigned col = 0; col < THEIA_COLS; ) {
            unsigned first = col;
            uint8_t attr = p->committed[row * THEIA_COLS + col] >> 8;
            payload[0] = 3; payload[1] = row; payload[2] = col; payload[3] = attr;
            do {
                payload[4 + col - first] = p->committed[row * THEIA_COLS + col] & 0xffu;
                col++;
            } while (col < THEIA_COLS && (p->committed[row * THEIA_COLS + col] >> 8) == attr);
            unsigned length = 4u + col - first;
            bytes += span(length);
            if (position) {
                record(p, position, payload, length);
            }
        }
    }
    if (position) {
        payload[0] = 4; record(p, position, payload, 1);
    }
    return bytes;
}
static bool sendScreen(theiaProducer_t *p)
{
    if (!theiaProducerReady(p)) {
        p->wasReady = false; p->dirty = true; return false;
    }
    uint32_t resync = *p->mailbox.resync;
    acquire();
    if (!p->wasReady || resync != p->seenResync) {
        p->dirty = true;
    }
    p->wasReady = true;
    if (!p->dirty) {
        return true;
    }
    uint32_t position;
    if (!reserve(p, screen(p, NULL), &position)) {
        return false;
    }
    screen(p, &position);
    publish(p, position);
    p->dirty = false; p->seenResync = resync;
    return true;
}
bool theiaProducerDraw(theiaProducer_t *p)
{
    if (p->staged) {
        if (memcmp(p->committed, p->cells, sizeof(p->committed))) {
            memcpy(p->committed, p->cells, sizeof(p->committed));
            p->dirty = true;
        }
        p->staged = false;
    }
    p->released = false;
    return sendScreen(p);
}
bool theiaProducerHeartbeat(theiaProducer_t *p)
{
    if (p->released) {
        return command(p, 1);
    }
    if (!sendScreen(p)) {
        return false;
    }
    return command(p, 0);
}
bool theiaProducerRelease(theiaProducer_t *p)
{
    p->released = true; p->dirty = true;
    return command(p, 1);
}
bool theiaProducerFontWrite(theiaProducer_t *p, uint16_t glyph, const uint8_t bits[54])
{
    if (glyph >= H415C_FONT_GLYPHS || p->fontOutstanding) {
        return false;
    }
    const uint32_t generation = *p->mailbox.resync;
    acquire();
    uint32_t position;
    if (!reserve(p, span(68), &position)) {
        return false;
    }
    uint8_t payload[68] = { 7, glyph & 255u, glyph >> 8, 0 };
    memcpy(payload + 4, bits, 54);
    const uint32_t sequence = p->fontSeq + 1u;
    for (unsigned i = 0; i < 4; i++) {
        payload[58 + i] = (uint8_t)(sequence >> (8u * i));
    }
    record(p, &position, payload, sizeof(payload));
#ifdef THEIA_PRODUCER_TEST
    extern void theiaProducerTestBeforeFontPublish(theiaProducer_t *producer);
    theiaProducerTestBeforeFontPublish(p);
#endif
    if (*p->mailbox.resync != generation) {
        return false;                                   /* Nothing published; retry is safe. */
    }
    p->fontSeq = sequence; p->fontOutstanding = true; p->fontResync = generation;
    publish(p, position);
    return true;
}
theiaFontStatus_e theiaProducerFontStatus(theiaProducer_t *p)
{
    if (!p->fontOutstanding) {
        return THEIA_FONT_ERROR;
    }
    /* Consumer publishes the glyph completion before later unrelated
     * display errors. A precise completion wins over global RESYNC. */
    uint32_t generation = *p->mailbox.resync;
    acquire();
    uint32_t done = *p->mailbox.fontDoneSeq;
    if (done == p->fontSeq) {
        p->fontOutstanding = false;
        return THEIA_FONT_DONE;
    }
    if (generation != p->fontResync) {
        p->fontOutstanding = false;
        return THEIA_FONT_ERROR;
    }
    return THEIA_FONT_PENDING;
}
void theiaProducerFontCancel(theiaProducer_t *p)
{
    /* Never reuse the sequence of an operation that might still be queued. */
    p->fontOutstanding = false;
}
