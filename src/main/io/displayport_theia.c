/* GPL-3.0-or-later. H415 internal OSD uses the shared-memory ABI only. */
#include <stdbool.h>
#include <stdint.h>
#include "platform.h"
#if defined(CH32H4) && defined(USE_THEIA_OSD)
#include <string.h>
#include "drivers/display.h"
#include "drivers/osd.h"
#include "drivers/time.h"
#include "fc/runtime_config.h"
#include "io/displayport_theia.h"
#include "io/h415_contract.h"
#include "io/displayport_theia_ring.h"

static displayPort_t display;
static theiaProducer_t producer;
static bool initialized;
static int fontOwner;
static uint16_t fontGlyph;
static uint8_t fontData[54];
static timeUs_t fontStartedAt;
#define WORD(address) ((volatile uint32_t *)(address))
static void geometry(displayPort_t *port)
{
    port->cols = THEIA_COLS;
    port->rows = *producer.mailbox.videoFormat == H415C_VIDEO_NTSC ? VIDEO_LINES_NTSC : VIDEO_LINES_PAL;
}
static int heartbeat(displayPort_t *port)
{
    geometry(port);
    if (theiaProducerPollHealth(&producer, micros())) {
        (void)theiaProducerHeartbeat(&producer);
    }
    return 0;
}
static int grab(displayPort_t *port)
{
    geometry(port);
    return 0; /* The caller clears and draws its new content before commit. */
}
static int release(displayPort_t *port)
{
    (void)port; return theiaProducerRelease(&producer) ? 0 : -1;
}
static int clearScreen(displayPort_t *port, displayClearOption_e options)
{
    (void)port; (void)options; theiaProducerClear(&producer); return 0;
}
static bool drawScreen(displayPort_t *port)
{
    (void)port;
    if (!theiaProducerPollHealth(&producer, micros())) {
        theiaProducerCommit(&producer);
        return false;
    }
    /* BF true means more transfer work remains. Never block FC startup
     * on an absent/incompatible OSD image; retain the shadow for recovery. */
    bool sent = theiaProducerDraw(&producer);
    return theiaProducerReady(&producer) && !sent;
}
static void commit(displayPort_t *port)
{
    (void)drawScreen(port);
}
static int screenSize(const displayPort_t *port)
{
    return port->cols * port->rows;
}
static int writeString(displayPort_t *port, uint8_t col, uint8_t row, uint8_t attr, const char *text)
{
    (void)port;
    /* Severity font banks are deliberately ignored, matching the shipped
     * H415 BF profile. Blink is an explicit ABI-v4 improvement. */
    theiaProducerWrite(&producer, col, row, (attr & DISPLAYPORT_BLINK) ? 0x40 : 0,
                       (const uint8_t *)text, strlen(text));
    return 0;
}
static int writeChar(displayPort_t *port, uint8_t col, uint8_t row, uint8_t attr, uint8_t glyph)
{
    (void)port;
    theiaProducerWrite(&producer, col, row, (attr & DISPLAYPORT_BLINK) ? 0x40 : 0, &glyph, 1);
    return 0;
}
static int writeSys(displayPort_t *port, uint8_t col, uint8_t row, displayPortSystemElement_e element)
{
    /* Goggles-side system elements have no analog OSD equivalent. */
    (void)port; (void)col; (void)row; (void)element; return -1;
}
static bool transfer(const displayPort_t *port)
{
    (void)port; return false;
}
static bool synced(const displayPort_t *port)
{
    (void)port;
    return producer.healthOnline && theiaProducerReady(&producer) && *producer.mailbox.resync == producer.seenResync &&
        !producer.dirty && !producer.staged;
}
static void redraw(displayPort_t *port)
{
    producer.dirty = true; (void)drawScreen(port);
}
static uint32_t bytesFree(const displayPort_t *port)
{
    (void)port; return theiaProducerBytesFree(&producer);
}
static displayFontResult_e fontWrite(displayPort_t *port, uint16_t glyph, const osdCharacter_t *chr, int owner)
{
    (void)port;
    if (!chr || glyph >= H415C_FONT_GLYPHS) {
        return DISPLAY_FONT_ERROR;
    }
    if (producer.fontOutstanding) {
        if (owner != fontOwner || glyph != fontGlyph || memcmp(fontData, chr->data, sizeof(fontData))) {
            return DISPLAY_FONT_ERROR;
        }
        const theiaFontStatus_e status = theiaProducerFontStatus(&producer);
        if (status == THEIA_FONT_DONE) {
            return DISPLAY_FONT_DONE;
        }
        if (status == THEIA_FONT_ERROR) {
            return DISPLAY_FONT_ERROR;
        }
        if (ARMING_FLAG(ARMED) || (timeUs_t)(micros() - fontStartedAt) >= 1000000u) {
            theiaProducerFontCancel(&producer);
            return DISPLAY_FONT_ERROR;
        }
        return DISPLAY_FONT_PENDING;
    }
    if (ARMING_FLAG(ARMED) || !theiaProducerFontWrite(&producer, glyph, chr->data)) {
        return DISPLAY_FONT_ERROR;
    }
    fontOwner = owner;
    fontGlyph = glyph;
    memcpy(fontData, chr->data, sizeof(fontData));
    fontStartedAt = micros();
    return DISPLAY_FONT_PENDING;
}
static void cancelFontWrite(displayPort_t *port, int owner)
{
    (void)port;
    if (producer.fontOutstanding && owner == fontOwner) {
        theiaProducerFontCancel(&producer);
    }
}
static bool ready(displayPort_t *port, bool rescan)
{
    (void)rescan; geometry(port); return theiaProducerPollHealth(&producer, micros());
}
static const displayPortVTable_t vtable = {
    .grab = grab, .release = release, .clearScreen = clearScreen, .drawScreen = drawScreen,
    .screenSize = screenSize, .writeSys = writeSys, .writeString = writeString, .writeChar = writeChar,
    .isTransferInProgress = transfer, .heartbeat = heartbeat, .redraw = redraw, .isSynced = synced,
    .txBytesFree = bytesFree, .writeFontCharacterAsync = fontWrite,
    .cancelFontWrite = cancelFontWrite, .checkReady = ready,
    .commitTransaction = commit,
};
displayPort_t *displayPortTheiaInit(void)
{
    if (initialized) {
        return &display;
    }
    const theiaMailbox_t mailbox = {
        .ring = (volatile uint8_t *)H415C_SHRAM_DP_RING,
        .writePos = WORD(H415C_SHRAM_DP_WRITE_POS), .readPos = WORD(H415C_SHRAM_DP_READ_POS),
        .bootState = WORD(H415C_SHRAM_BOOT_STATE), .hwReady = WORD(H415C_SHRAM_HW_READY),
        .abiVersion = WORD(H415C_SHRAM_ABI_VERSION), .resync = WORD(H415C_SHRAM_DP_RESYNC),
        .videoFormat = WORD(H415C_SHRAM_VIDEO_FORMAT), .fontDoneSeq = WORD(H415C_SHRAM_FONT_DONE_SEQ),
    };
    theiaProducerInit(&producer, &mailbox);
    displayInit(&display, &vtable, DISPLAYPORT_DEVICE_TYPE_THEIA);
    display.useDeviceBlink = true;
    geometry(&display); initialized = true;
    return &display;
}
#endif
