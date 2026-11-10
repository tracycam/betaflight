/*
 * h415_contract.h - canonical V3F(BL)/V5F(APP)/OSD-APP shared contract for
 * the H415 flight controller. CANONICAL COPY lives in TheiaOSD repo
 * (targets/h415_v3f/h415_contract.h); this twin MUST be kept byte-identical.
 * Verify with: cmp against the H415_FC_Bootloader and BF_H417 twins.
 * Version history: v1 2026-08-27 initial partition + mailbox definitions.
 *                 v2 2026-08-27 font page layout (payload@+256), DP ring
 *                 ABI (MSP-v1 records), staging ownership = V3 OSD APP.
 *                 v3 2026-08-27 DP ABI fence order fixed (consume THEN
 *                 publish), record span formula, BL zeroes mailbox epoch
 *                 before waking V5F; DFU regions are alternates of one
 *                 interface (DfuSe-correct).
 *                 v4 2026-10-23 paired OSD/BF ABI v4 (HW_READY, ABI_VERSION,
 *                 FONT_DONE_SEQ, FONT_COMMIT_RESULT mailbox words, APP
 *                 header abi_version check); DFU alt 2 = read-only Option
 *                 Bytes shim for host GCS RDP pre-checks.
 */
#ifndef H415_CONTRACT_H
#define H415_CONTRACT_H
#define H415C_VERSION 4u

#include <stdint.h>


/* ---------------- Flash partition (programmer/DFU address space) -------------
 * ALL addresses in this contract are programmer-space (0x0800xxxx), and
 * images LINK AND RUN in this same space: the OSD APP links its XIP image
 * at 0x08006020 (= H415C_OSD_BASE + H415C_APP_HDR_SIZE, after the image
 * header) and the BL enters it exactly there. Do NOT apply a
 * "value - 0x08000000" translation. The ONE exception is the BL itself,
 * which must link at 0x00000000 because the V3F core boots at PC=0; the
 * 0x0800xxxx window is otherwise mapped and fully functional.
 * All region bases are aligned to the 8K erase unit.
 * Total chip flash 960K. Layout:
 *   BL   0x08000000 .. 0x08006000  (24K, frozen, never field-updatable)
 *   OSD  0x08006000 .. 0x0800E000  (32K, TheiaOSD h415_v3f image)
 *   FONT 0x0800E000 .. 0x08016000  (32K, 4 x 8K sectors, font envelope NVM)
 *   V5F  0x08016000 .. 0x080F0000  (872K, flight controller APP)
 */
#define H415C_BL_BASE         0x08000000u
#define H415C_BL_SIZE          (24u * 1024u)
#define H415C_OSD_BASE         0x08006000u
#define H415C_OSD_SIZE         (32u * 1024u)
#define H415C_FONT_BASE        0x0800E000u
#define H415C_FONT_SIZE        (32u * 1024u)
#define H415C_V5F_BASE         0x08016000u
#define H415C_V5F_SIZE         ((960u - 88u) * 1024u)
#define H415C_SECTOR_SIZE      0x2000u

/* ---------------- Shared RAM (visible to both cores) -------------------------
 * V3F private RAM ends exactly at 0x20118000; everything below this line is
 * inter-core and MUST be declared NOLOAD in both linker scripts.
 */
#define H415C_SHRAM_FONT_STAGING       0x20118000u
#define H415C_SHRAM_FONT_STAGING_SIZE  (32u * 1024u)
#define H415C_SHRAM_FONT_MAGIC         0x20120000u /* request magic, written by V3 OSD APP */
#define H415C_SHRAM_FONT_MAGIC_LEGACY  0x1234u     /* vendor-compatible trigger */
#define H415C_SHRAM_FONT_ADDR_LEGACY   0x20120004u /* legacy target addr, now unused */
#define H415C_SHRAM_BOOT_STATE         0x20120008u /* h415c_boot_state_t by V3F BL */
#define H415C_SHRAM_DP_RING            0x20120100u /* 16K MSP-DP ring, V5F produces */
#define H415C_SHRAM_DP_RING_SIZE       (16u * 1024u)

/* V5F wakeup entry handed to NVIC_WakeUp_V5F (V3F bus address space). */
#define H415C_V5F_ENTRY                0x00016000u

/* ---------------- OSD APP image header (flash @ H415C_OSD_BASE) -------------- */
#define H415C_APP_MAGIC   0x4F534431u /* "OSD1" */
typedef struct {
    uint32_t magic;      /* H415C_APP_MAGIC */
    uint32_t version;    /* image format version (this header: 1) */
    uint32_t entry_off;  /* trampoline offset from H415C_OSD_BASE */
    uint32_t load_len;   /* image byte count AFTER the 32-byte header */
    uint32_t crc32;      /* CRC32 over [header_end .. header_end+load_len) */
    uint32_t flags;      /* bit0: valid marker, written by flash tool */
    uint32_t abi_version; /* required transport/resource ABI, checked before jump */
    uint32_t reserved;
} h415c_app_hdr_t; /* fixed 32 bytes, packed by construction */
#define H415C_APP_HDR_SIZE 32u
/* ---------------- Static layout guarantees -------------------------------- */
#if defined(__STDC_VERSION__)
_Static_assert(sizeof(h415c_app_hdr_t) == 32u, "app header must be 32B");
_Static_assert(H415C_BL_BASE + H415C_BL_SIZE == H415C_OSD_BASE, "partition gap");
_Static_assert(H415C_OSD_BASE + H415C_OSD_SIZE == H415C_FONT_BASE, "partition gap");
_Static_assert(H415C_FONT_BASE + H415C_FONT_SIZE == H415C_V5F_BASE, "partition gap");
_Static_assert(H415C_V5F_BASE + H415C_V5F_SIZE == 0x080F0000u, "flash end mismatch");
_Static_assert((H415C_OSD_BASE % H415C_SECTOR_SIZE) == 0u, "OSD not sector aligned");
_Static_assert((H415C_FONT_BASE % H415C_SECTOR_SIZE) == 0u, "FONT not sector aligned");
#endif

/* ---------------- Font envelope (flash @ H415C_FONT_BASE) --------------------
 * PHYSICAL LAYOUT (256B page granularity of FLASH_ProgramPage_Fast):
 *   page 0 (FONT_BASE..+255)     : 32B header, rest of page UNUSED
 *   page 1.. (FONT_BASE+256...)  : payload, 27648 bytes
 * Commit order (power-fail safe, single copy): compute staging CRC ->
 * consume request magic -> erase 4 sectors -> program payload pages ->
 * verify readback -> program header page LAST -> verify header. A torn commit
 * leaves an invalid envelope; recovery is re-upload (accepted trade-off).
 *
 * OWNERSHIP (legacy-compatible): the V3 OSD APP owns the staging buffer:
 * it seeds staging from the current valid envelope, applies glyph updates
 * arriving over the paired ring (MSP-DP sub 7), and writes the request magic LAST.
 * The V5F/BF side does NOT write staging. The BL consumes the request at
 * boot. The legacy 0x1234 magic is a producer-trusted (unchecked) path.
 *
 * DEFAULT FONT: missing or invalid FNT1 uses the complete compressed
 * in-image vendor font, expanded into staging. Boot does not request a
 * commit or reset the whole chip: a failed first-boot commit must never
 * prevent the flight core from booting. A received glyph upload updates
 * this live page and raises FONT_MAGIC; the BL commits it at the next
 * ordinary soft reset. A power loss before that commit loses the upload.
 * The OSD APP never writes flash itself; BL remains the only flash writer.
 */
#define H415C_FONT_ENVELOPE_MAGIC 0x464E5431u /* "FNT1" */
#define H415C_FONT_GLYPHS          512u
#define H415C_FONT_BYTES_PER_GLYPH 54u
#define H415C_FONT_HDR_SIZE        32u
#define H415C_FONT_PAYLOAD_OFF     256u  /* payload starts at page 1 */
#define H415C_FONT_PAYLOAD_LEN \
    (H415C_FONT_GLYPHS * H415C_FONT_BYTES_PER_GLYPH)
typedef struct {
    uint32_t magic;      /* H415C_FONT_ENVELOPE_MAGIC, programmed last */
    uint32_t version;    /* envelope format version (this header: 1) */
    uint16_t glyph_count;
    uint16_t bytes_per_glyph;
    uint32_t encoding;   /* glyph encoding id, 0 = MAX7456 12x18 2bpmc */
    uint32_t payload_len;
    uint32_t crc32;      /* CRC32 over payload bytes */
    uint32_t reserved[2];
} h415c_font_hdr_t; /* fixed 32 bytes, occupies page 0 alone */
#if defined(__STDC_VERSION__)
_Static_assert(sizeof(h415c_font_hdr_t) == 32u, "font header must be 32B");
_Static_assert((H415C_FONT_PAYLOAD_OFF % 256u) == 0u, "payload not page aligned");
_Static_assert(H415C_FONT_PAYLOAD_OFF + H415C_FONT_PAYLOAD_LEN <= H415C_FONT_SIZE, "font overflow");
_Static_assert((H415C_FONT_PAYLOAD_LEN % 256u) == 0u, "payload must be whole pages");
#endif

/* ---------------- Paired OSD/BF ABI v4 -------------------------------------
 * BL owns shared RCC gates and the OSD GPIO/AFIO configuration. It completes
 * those writes BEFORE V5F wakeup, then publishes HW_READY and ABI_VERSION.
 * Afterwards V3F only changes its exclusive timers/DMA/OPA and uses BSHR
 * for owned output bits; V5F must never assign the reserved OSD resources.
 * OSD-disabled strap is represented by a clear HW_OSD_ENABLED bit.
 * Pixel DMA uses word-wide GPIOB BSHR samples restricted to bits0/1/7;
 * it must never write OUTDR or change the other GPIOB latch bits.
 *
 * Single V5F producer / single V3F consumer. Each ring record contains one
 * complete MSPv1 DISPLAYPORT frame ('$M>', length, 0xB6, payload, XOR CRC).
 * u16BE record length; span = align4(2 + length). Payload is 1..192 bytes.
 * Consumer validates the entire record BEFORE dispatch, never searches
 * within a rejected record for another frame. No HSEM or IPC_MSG cursor.
 *
 * write_pos and read_pos are monotonic u32 counters. Producer writes all
 * bytes, then release fence, then publishes write_pos. Consumer acquires
 * write_pos before copying; releases read_pos only after processing each
 * private record. Unsigned lag is <=16K. Neither side overwrites the other
 * side's cursor. BL initializes both before waking V5F.
 *
 * Producer publishes CLEAR + all text + DRAW as ONE atomic ring batch,
 * reserving the entire batch before writing; a full ring rejects the whole
 * batch without blocking the flight loop. Consumer corruption discards the
 * remaining snapshot and increments DP_RESYNC. Producer retains its local
 * display state and republishes a complete batch on readiness/resync.
 * Heartbeats keep a static display alive and retry pending redraws.
 *
 * One font operation at a time. The 68-byte DP font payload carries a
 * little-endian operation sequence in metadata bytes [58..61], after the
 * 54 visible glyph bytes. OSD publishes FONT_DONE_SEQ only after applying
 * that glyph. BF acknowledges the originating MSP request after completion;
 * display RESYNC cannot revoke a completed font operation. FONT_DONE_SEQ
 * is initialized by BL and thereafter written only by OSD. Sequence numbers
 * are not ring cursors and must not be reused after a request timeout.
 * Font staging remains volatile until the ordinary BL commit; each glyph
 * write withdraws FONT_MAGIC,
 * fences, changes 54 bytes, fences, then publishes FONT_MAGIC. Reset during
 * mutation aborts all uncommitted changes, retaining the last flash font.
 */
#define H415C_ABI_VERSION             4u
#define H415C_SHRAM_HW_READY          0x20120018u
#define H415C_HW_READY_MAGIC          0x48574400u
#define H415C_HW_OSD_ENABLED          1u
#define H415C_SHRAM_DP_RESYNC         0x2012001cu
#define H415C_SHRAM_ABI_VERSION       0x20120020u
#define H415C_SHRAM_VIDEO_FORMAT      0x20120024u
#define H415C_SHRAM_FONT_DONE_SEQ     0x20120028u
#define H415C_SHRAM_FONT_COMMIT_RESULT 0x2012002cu /* signed BL commit result, diagnostic */
#define H415C_SHRAM_STAGE_TRACE       0x20120030u /* V3F bring-up stage word, diagnostic */
#define H415C_SHRAM_FAULT_MARKER      0x20120038u /* V3F trap marker, diagnostic */
#define H415C_VIDEO_NONE              0u
#define H415C_VIDEO_PAL               1u
#define H415C_VIDEO_NTSC              2u
#define H415C_DP_MAX_PAYLOAD          192u
#define H415C_DP_MAX_RECORD           (H415C_DP_MAX_PAYLOAD + 6u)
#define H415C_SHRAM_DP_WRITE_POS 0x20120010u /* u32, producer publishes */
#define H415C_SHRAM_DP_READ_POS  0x20120014u /* u32, consumer publishes */
/* ---------------- Boot state mailbox (@ H415C_SHRAM_BOOT_STATE) -------------- */
typedef enum {
    H415C_BS_V3_BOOTING     = 0xA11C0001u,
    H415C_BS_V3_OSD_RUNNING = 0xA11C0002u, /* DP reader initialized; see DP ABI */
    H415C_BS_V3_OSD_INVALID = 0xA11C0003u,
    H415C_BS_V3_DFU         = 0xA11C0004u,
    H415C_BS_V3_OSD_FAULT   = 0xA11C0005u, /* runtime failure; producer must stop */
} h415c_boot_state_e;

/* ---------------- DFU region assignment ---------------------------------------
 * DfuSe-correct shape: ONE DFU interface (bInterfaceNumber 0) with three
 * bAlternateSetting values, selected by the host via standard SET_INTERFACE
 * (this is what dfu-util -a N and DfuSe hosts send; wIndex stays 0 for all
 * class requests). Each alternate names its region via an iInterface
 * DfuSe memory-layout string.
 */
#define H415C_DFU_ALT_V5F 0u /* region: V5F APP   @ H415C_V5F_BASE  */
#define H415C_DFU_ALT_OSD 1u /* region: OSD image @ H415C_OSD_BASE  */
#define H415C_DFU_ALT_OB  2u /* host-GCS shim (NOT flash): read-only fake
                             * Option Bytes window @ 0x1FFFF800, RAM-served,
                             * only to satisfy the INAV / BF<=10.10 RDP
                             * pre-check (UPLOAD 16B must succeed). OB
                             * programming is NOT supported. */

#endif /* H415_CONTRACT_H */
