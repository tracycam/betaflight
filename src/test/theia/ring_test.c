/* Host test of the exact BF producer; no emulated producer implementation. */
#include "io/displayport_theia_ring.h"
#include "io/h415_contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t ring[H415C_SHRAM_DP_RING_SIZE];
static uint32_t writePos, readPos, bootState, hwReady, abiVersion, resync, videoFormat, fontDoneSeq;
static theiaProducer_t producer;
static const theiaMailbox_t mailbox = { ring, &writePos, &readPos, &bootState, &hwReady,
                                        &abiVersion, &resync, &videoFormat, &fontDoneSeq };
static uint16_t cells[480];
static unsigned draws, releases, fonts;
#ifdef THEIA_PRODUCER_TEST
static bool discardBeforePublish;
void theiaProducerTestBeforeFontPublish(theiaProducer_t *p)
{
    if (discardBeforePublish) {
        /* Consumer discards its already published snapshot while BF is
        * writing a second glyph, after FontPending's initial check. */
        ++*p->mailbox.resync;
        *p->mailbox.readPos = *p->mailbox.writePos;
        discardBeforePublish = false;
    }
}
#endif
static uint8_t next(uint32_t *position)
{
    return ring[(*position)++ & (sizeof(ring) - 1)];
}
static void consume(void)
{
    uint32_t position = readPos;
    while (position != writePos) {
        uint32_t start = position;
        unsigned length = (unsigned)next(&position) << 8; length |= next(&position);
        assert(length <= H415C_DP_MAX_RECORD);
        assert(next(&position) == '$' && next(&position) == 'M' && next(&position) == '>');
        unsigned size = next(&position); assert(size + 6 == length && next(&position) == 0xb6);
        uint8_t crc = size ^ 0xb6, payload[192];
        for (unsigned i = 0; i < size; i++) {
            payload[i] = next(&position); crc ^= payload[i];
        }
        assert(next(&position) == crc);
        if (payload[0] == 2) {
            memset(cells, 0, sizeof(cells));
        }
        if (payload[0] == 3) {
            unsigned cell = payload[1] * 30 + payload[2];
            for (unsigned i = 4; i < size; i++) {
                cells[cell++] = payload[i] | ((uint16_t)payload[3] << 8);
            }
        }
        if (payload[0] == 4) {
            draws++;
        }
        if (payload[0] == 1) {
            releases++;
        }
        if (payload[0] == 7) {
            assert(size == 68 && payload[4] == 0xa5); fonts++;
            fontDoneSeq = 0;
            for (unsigned i = 0; i < 4; i++) {
                fontDoneSeq |= (uint32_t)payload[58 + i] << (8 * i);
            }
        }
        position = start + ((length + 5u) & ~3u);
    }
    readPos = position;
}
int main(void)
{
    theiaProducerInit(&producer, &mailbox);
    const uint8_t name[] = { 'T', 'H', 'E', 'I', 'A' };
    theiaProducerWrite(&producer, 7, 2, 0x40, name, sizeof(name));
    assert(!theiaProducerDraw(&producer) && !writePos); /* absent OSD */
    bootState = H415C_BS_V3_OSD_RUNNING; hwReady = H415C_HW_READY_MAGIC | 1; abiVersion = 2;
    assert(!theiaProducerHeartbeat(&producer) && !writePos); /* incompatible ABI */
    abiVersion = H415C_ABI_VERSION;
    assert(theiaProducerHeartbeat(&producer)); consume();
    assert(draws == 1 && cells[67] == ('T' | 0x4000)); /* late ready retained frame */
    /* A heartbeat/resync must not publish writes before their DRAW. */
    uint8_t changed = 'X'; theiaProducerWrite(&producer, 7, 2, 0, &changed, 1);
    resync++; assert(theiaProducerHeartbeat(&producer)); consume();
    assert(cells[67] == ('T' | 0x4000)); draws = 1;
    /* Backpressure reserves the entire worst-case frame before writing. */
    for (unsigned r = 0; r < 16; r++) {
        for (unsigned c = 0; c < 30; c++) {
            uint8_t glyph = (uint8_t)(c + 1); theiaProducerWrite(&producer, c, r, (c & 1) ? 0x40 : 0, &glyph, 1);
        }
    }
    uint32_t actual = writePos; readPos = writePos - sizeof(ring) + 4;
    uint8_t snapshot[sizeof(ring)]; memcpy(snapshot, ring, sizeof(ring));
    assert(!theiaProducerDraw(&producer)); assert(writePos == actual && !memcmp(snapshot, ring, sizeof(ring)));
    readPos = writePos; assert(theiaProducerDraw(&producer)); consume();
    assert(draws == 2 && !memcmp(cells, producer.cells, sizeof(cells)));
    resync++; assert(theiaProducerHeartbeat(&producer)); consume(); assert(draws == 3);
    assert(theiaProducerRelease(&producer)); consume(); assert(releases == 1);
    assert(theiaProducerHeartbeat(&producer)); consume(); assert(releases == 2 && draws == 3);
    /* Both ring offset and monotonic u32 cursor wrap. */
    writePos = readPos = UINT32_MAX - 31; producer.dirty = true;
    assert(theiaProducerDraw(&producer)); consume(); assert(!memcmp(cells, producer.cells, sizeof(cells)));
    /* An unchanged final screen after CLEAR+rewrite produces no new batch. */
    actual = writePos; theiaProducerClear(&producer);
    for (unsigned r = 0; r < 16; r++) {
        for (unsigned c = 0; c < 30; c++) {
            uint8_t value = (uint8_t)(c + 1); theiaProducerWrite(&producer, c, r, (c & 1) ? 0x40 : 0, &value, 1);
        }
    }
    assert(theiaProducerDraw(&producer) && writePos == actual);
    uint8_t glyph[54]; memset(glyph, 0xa5, sizeof(glyph));
    assert(!theiaProducerFontWrite(&producer, 512, glyph));
    assert(theiaProducerFontWrite(&producer, 511, glyph));
    assert(theiaProducerFontStatus(&producer) == THEIA_FONT_PENDING);
    assert(!theiaProducerFontWrite(&producer, 510, glyph)); /* exactly one operation */
    consume(); resync++; /* Later display errors cannot revoke exact font completion. */
    assert(fonts == 1 && theiaProducerFontStatus(&producer) == THEIA_FONT_DONE);
    assert(theiaProducerFontWrite(&producer, 0, glyph)); resync++; readPos = writePos;
    assert(theiaProducerFontStatus(&producer) == THEIA_FONT_ERROR);
    assert(theiaProducerFontWrite(&producer, 0, glyph)); /* No permanent failure latch. */
    uint32_t cancelled = producer.fontSeq; theiaProducerFontCancel(&producer);
    assert(theiaProducerFontWrite(&producer, 1, glyph));
    fontDoneSeq = cancelled; assert(theiaProducerFontStatus(&producer) == THEIA_FONT_PENDING);
    consume(); assert(theiaProducerFontStatus(&producer) == THEIA_FONT_DONE);
    producer.fontSeq = fontDoneSeq = UINT32_MAX;
    assert(theiaProducerFontWrite(&producer, 1, glyph)); assert(producer.fontSeq == 0);
    assert(theiaProducerFontStatus(&producer) == THEIA_FONT_PENDING);
    consume(); assert(theiaProducerFontStatus(&producer) == THEIA_FONT_DONE);
#ifdef THEIA_PRODUCER_TEST
    discardBeforePublish = true; actual = writePos;
    assert(!theiaProducerFontWrite(&producer, 1, glyph)); assert(writePos == actual);
    assert(theiaProducerFontWrite(&producer, 1, glyph)); consume();
    assert(theiaProducerFontStatus(&producer) == THEIA_FONT_DONE);
#endif
    /* A probe needs actual consumer progress, not just RUNNING in the mailbox. */
    writePos = readPos = resync = 0;
    theiaProducerInit(&producer, &mailbox);
    assert(!theiaProducerPollHealth(&producer, 0));
    assert(producer.healthPending && writePos != readPos);
    consume(); assert(theiaProducerPollHealth(&producer, 1));
    assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_INTERVAL_US + 1));
    assert(producer.healthPending);
    assert(!theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US + 1));
    assert(producer.dirty && !producer.wasReady);
    consume(); assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US + 2));
    assert(theiaProducerHeartbeat(&producer)); consume(); /* recovery republishes */

    /* Discarded records never acknowledge a probe, even if the cursor advances. */
    assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US + THEIA_HEALTH_INTERVAL_US + 2));
    resync++; readPos = writePos;
    assert(!theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 2));
    assert(producer.healthPending); consume();
    assert(theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 3));

    /* Both monotonic cursor and BF time wrap remain safe. */
    theiaProducerInit(&producer, &mailbox);
    writePos = readPos = UINT32_MAX - 11u;
    assert(!theiaProducerPollHealth(&producer, UINT32_MAX - 100u));
    consume(); assert(theiaProducerPollHealth(&producer, UINT32_MAX - 99u));
    assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_INTERVAL_US));
    assert(!theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US));
    consume(); assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US + 1));
    /* A full ring prevents new probes; a stale last ACK still expires. */
    readPos = writePos - H415C_SHRAM_DP_RING_SIZE;
    assert(theiaProducerPollHealth(&producer, THEIA_HEALTH_TIMEOUT_US + THEIA_HEALTH_INTERVAL_US + 1));
    assert(!producer.healthPending);
    assert(!theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 1));
    readPos = writePos;
    assert(!theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 2));
    consume(); assert(theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 3));
    abiVersion = 2;
    assert(!theiaProducerPollHealth(&producer, 2 * THEIA_HEALTH_TIMEOUT_US + 4));
    assert(!producer.healthPending && !producer.healthOnline);
    puts(
        "THEIA ABI4: screen, resync, font and heartbeat acknowledgement/recovery pass");
}
