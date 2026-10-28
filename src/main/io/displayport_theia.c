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
#include "io/displayport_theia_abi.h"
#include "io/displayport_theia_ring.h"

static displayPort_t display;
static theiaProducer_t producer;
static bool initialized;
#define WORD(address) ((volatile uint32_t *)(address))
static void geometry(displayPort_t *port)
{
    port->cols = THEIA_COLS;
    port->rows = *producer.mailbox.videoFormat == H415C_VIDEO_NTSC ? VIDEO_LINES_NTSC : VIDEO_LINES_PAL;
}
static int heartbeat(displayPort_t *port)
{
    geometry(port);
    (void)theiaProducerHeartbeat(&producer);
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
    (void)port; (void)col; (void)row; (void)element; return 0;
}
static bool transfer(const displayPort_t *port)
{
    (void)port; return false;
}
static bool synced(const displayPort_t *port)
{
    (void)port; return !producer.dirty && !producer.staged;
}
static void redraw(displayPort_t *port)
{
    producer.dirty = true; (void)drawScreen(port);
}
static uint32_t bytesFree(const displayPort_t *port)
{
    (void)port; return theiaProducerBytesFree(&producer);
}
static bool fontWrite(displayPort_t *port, uint16_t glyph, const osdCharacter_t *chr)
{
    (void)port;
    if (ARMING_FLAG(ARMED)) {
        return false;
    }
    if (glyph >= H415C_FONT_GLYPHS || !theiaProducerFontWrite(&producer, glyph, chr->data)) {
        return false;
    }

    const timeUs_t startedAt = micros();
    while (micros() - startedAt < 1000000) {
        const theiaFontStatus_e status = theiaProducerFontStatus(&producer);
        if (status == THEIA_FONT_DONE) {
            return true;
        }
        if (status == THEIA_FONT_ERROR) {
            break;
        }
        delayMicroseconds(100);
    }

    theiaProducerFontCancel(&producer);
    return false;
}
static bool ready(displayPort_t *port, bool rescan)
{
    (void)rescan; geometry(port); return theiaProducerReady(&producer);
}
static const displayPortVTable_t vtable = {
    .grab = grab, .release = release, .clearScreen = clearScreen, .drawScreen = drawScreen,
    .screenSize = screenSize, .writeSys = writeSys, .writeString = writeString, .writeChar = writeChar,
    .isTransferInProgress = transfer, .heartbeat = heartbeat, .redraw = redraw, .isSynced = synced,
    .txBytesFree = bytesFree, .writeFontCharacter = fontWrite, .checkReady = ready,
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
