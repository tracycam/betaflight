/* TheiaOSD shared-memory ABI v4 for the H415 Bootloader. */
#pragma once

#define H415C_ABI_VERSION              4u
#define H415C_FONT_GLYPHS              512u

#define H415C_SHRAM_BOOT_STATE         0x20120008u
#define H415C_SHRAM_DP_WRITE_POS       0x20120010u
#define H415C_SHRAM_DP_READ_POS        0x20120014u
#define H415C_SHRAM_HW_READY           0x20120018u
#define H415C_SHRAM_DP_RESYNC          0x2012001cu
#define H415C_SHRAM_ABI_VERSION        0x20120020u
#define H415C_SHRAM_VIDEO_FORMAT       0x20120024u
#define H415C_SHRAM_FONT_DONE_SEQ      0x20120028u
#define H415C_SHRAM_DP_RING            0x20120100u
#define H415C_SHRAM_DP_RING_SIZE       (16u * 1024u)

#define H415C_HW_READY_MAGIC           0x48574400u
#define H415C_HW_OSD_ENABLED           1u
#define H415C_BS_V3_OSD_RUNNING        0xA11C0002u
#define H415C_VIDEO_NTSC               2u
#define H415C_DP_MAX_PAYLOAD           192u
#define H415C_DP_MAX_RECORD            (H415C_DP_MAX_PAYLOAD + 6u)
