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
#include <stdbool.h>
#include <stdint.h>

extern "C" {

#include "platform.h"
#include "target.h"

#include "drivers/accgyro/accgyro_spi_sch16t.h"

}

#include "unittest_macros.h"
#include "gtest/gtest.h"

// Golden vectors below are from the Murata SCH16T-K10 datasheet.
// CRC8: poly 0x2F, init 0xFF, bit-serial MSB-first over frame bits 47..0
// (low 8 bits zeroed), data bit enters at LSB after each shift.

// Read RATE_X1 (addr 0x001), genuine datasheet frame
static const uint64_t MOSI_READ_RATE_X1 = 0x0048000000ACULL;
// Read TEMP (addr 0x010), genuine datasheet frame
static const uint64_t MOSI_READ_TEMP = 0x0408000000B1ULL;

// Datasheet worked-example MISO frame: D=1, SA=0x001, S=00, DCNT=0,
// SENSOR=0xFFE00 (-512), CRC=0xAD (genuine)
static const uint64_t MISO_SENSOR_NEG512 = 0x80200FFE00ADULL;
// Positive SENSOR=0x00DC0 (3520); CRC byte is a placeholder, parse ignores it
static const uint64_t MISO_SENSOR_POS3520 = 0x8020000DC0DBULL;
// D=1, SA=0x001, S=00, DCNT=0, SENSOR=0x80000 (-524288), CRC=0xD6
static const uint64_t MISO_SENSOR_MIN = 0x8020080000D6ULL;

// MISO_SENSOR_NEG512 with each alternate S status, CRC recomputed with the
// datasheet algorithm so every status-class test frame remains CRC-valid
static const uint64_t MISO_STATUS_01 = 0x80220FFE00E8ULL; // S = 01
static const uint64_t MISO_STATUS_10 = 0x80240FFE0027ULL; // S = 10
static const uint64_t MISO_STATUS_11 = 0x80260FFE0062ULL; // S = 11

static const uint64_t MISO_D_BIT = 1ULL << 47;
static const uint64_t MISO_SA_MASK = 0x3FFULL << 37;
static const uint64_t MISO_CE_BIT = 1ULL << 35;
static const uint64_t MISO_STATUS_MASK = 0x3ULL << 33;

static uint64_t makeMisoFrame(uint64_t base, bool sensorData, uint16_t sourceAddress, uint8_t status, bool commandError)
{
    uint64_t frame = base & ~(MISO_D_BIT | MISO_SA_MASK | MISO_CE_BIT | MISO_STATUS_MASK | 0xFFULL);
    frame |= (uint64_t)sourceAddress << 37;
    frame |= (uint64_t)(status & 0x03U) << 33;
    if (sensorData) {
        frame |= MISO_D_BIT;
    }
    if (commandError) {
        frame |= MISO_CE_BIT;
    }

    return frame | sch16tCrc8(frame);
}

TEST(sch16tCrc8Test, GoldenMosiFrames)
{
    EXPECT_EQ(0xAC, sch16tCrc8(0x0048000000ACULL)); // read RATE_X1
    EXPECT_EQ(0xB1, sch16tCrc8(0x0408000000B1ULL)); // read TEMP
    EXPECT_EQ(0xC3, sch16tCrc8(0x0DA800000AC3ULL)); // write CTRL_RESET 0x0000A
    EXPECT_EQ(0xD3, sch16tCrc8(0x0D68000001D3ULL)); // write CTRL_MODE 0x00001
    EXPECT_EQ(0x8D, sch16tCrc8(0x0D680000038DULL)); // write CTRL_MODE 0x00003
    EXPECT_EQ(0x9B, sch16tCrc8(0x09680000DB9BULL)); // write CTRL_FILT_RATE 0x000DB
}

TEST(sch16tCrc8Test, GoldenMisoFrame)
{
    EXPECT_EQ(0xAD, sch16tCrc8(MISO_SENSOR_NEG512));
}

TEST(sch16tFrameReadTest, GoldenVectors)
{
    EXPECT_EQ(MOSI_READ_RATE_X1, sch16tFrameRead(0x001));
    EXPECT_EQ(MOSI_READ_TEMP, sch16tFrameRead(0x010));
}

TEST(sch16tFrameWriteTest, GoldenVectors)
{
    EXPECT_EQ(0x0DA800000AC3ULL, sch16tFrameWrite(SCH16T_CTRL_RESET, 0xA));  // soft reset
    EXPECT_EQ(0x0D68000001D3ULL, sch16tFrameWrite(SCH16T_CTRL_MODE, 0x1));   // enable sensor
    EXPECT_EQ(0x0D680000038DULL, sch16tFrameWrite(SCH16T_CTRL_MODE, 0x3));   // EOI
    EXPECT_EQ(0x09680000DB9BULL, sch16tFrameWrite(SCH16T_CTRL_FILT_RATE, 0xDB)); // LPF3 all axes
}

TEST(sch16tParseSensor20Test, NegativeDatasheetExample)
{
    EXPECT_EQ(-512, sch16tParseSensor20(MISO_SENSOR_NEG512));
}

TEST(sch16tParseSensor20Test, PositiveValue)
{
    EXPECT_EQ(3520, sch16tParseSensor20(MISO_SENSOR_POS3520));
}

TEST(sch16tParseSensor20Test, SignExtension)
{
    EXPECT_EQ(-524288, sch16tParseSensor20(MISO_SENSOR_MIN)); // SENSOR = 0x80000
}

TEST(sch16tParseSensor20Test, ShiftRight4Truncation)
{
    // runtime read path truncates 20-bit to 16-bit via >> 4
    EXPECT_EQ(-32, sch16tParseSensor20(MISO_SENSOR_NEG512) >> 4);
}

TEST(sch16tMisoCrcOkTest, ValidFrame)
{
    EXPECT_TRUE(sch16tMisoCrcOk(MISO_SENSOR_NEG512));
}

TEST(sch16tMisoCrcOkTest, CorruptedCrc)
{
    EXPECT_FALSE(sch16tMisoCrcOk(MISO_SENSOR_NEG512 ^ 0x01ULL));
}

TEST(sch16tRegisterFrameValidTest, InitializationStatusAccepted)
{
    const uint64_t frame = makeMisoFrame(MISO_SENSOR_NEG512, false, SCH16T_STAT_SUM, 0b11, false);
    EXPECT_TRUE(sch16tRegisterFrameValid(frame, SCH16T_STAT_SUM));
}

TEST(sch16tRegisterFrameValidTest, CommandErrorRejected)
{
    const uint64_t frame = makeMisoFrame(MISO_SENSOR_NEG512, false, SCH16T_STAT_SUM, 0b00, true);
    EXPECT_FALSE(sch16tRegisterFrameValid(frame, SCH16T_STAT_SUM));
}

TEST(sch16tRegisterFrameValidTest, SourceAddressMismatchRejected)
{
    const uint64_t frame = makeMisoFrame(MISO_SENSOR_NEG512, false, SCH16T_STAT_SUM, 0b00, false);
    EXPECT_FALSE(sch16tRegisterFrameValid(frame, SCH16T_STAT_COM));
}

TEST(sch16tRegisterFrameValidTest, SensorFrameRejected)
{
    EXPECT_FALSE(sch16tRegisterFrameValid(MISO_SENSOR_NEG512, SCH16T_RATE_X1));
}

TEST(sch16tSensorFrameValidTest, NormalFrameAccepted)
{
    EXPECT_TRUE(sch16tSensorFrameValid(MISO_SENSOR_NEG512, SCH16T_RATE_X1));
}

TEST(sch16tSensorFrameValidTest, SaturationAccepted)
{
    // Datasheet Table 20: S=10 is valid sensor data clipped at the configured range.
    EXPECT_TRUE(sch16tSensorFrameValid(MISO_STATUS_10, SCH16T_RATE_X1));
}

TEST(sch16tSensorFrameValidTest, ErrorAndInitializationRejected)
{
    EXPECT_FALSE(sch16tSensorFrameValid(MISO_STATUS_01, SCH16T_RATE_X1));
    EXPECT_FALSE(sch16tSensorFrameValid(MISO_STATUS_11, SCH16T_RATE_X1));
}

TEST(sch16tSensorFrameValidTest, CommandErrorRejected)
{
    const uint64_t frame = makeMisoFrame(MISO_SENSOR_NEG512, true, SCH16T_RATE_X1, 0b00, true);
    EXPECT_FALSE(sch16tSensorFrameValid(frame, SCH16T_RATE_X1));
}

TEST(sch16tSensorFrameValidTest, SourceAddressMismatchRejected)
{
    EXPECT_FALSE(sch16tSensorFrameValid(MISO_SENSOR_NEG512, SCH16T_RATE_Y1));
}

TEST(sch16tSensorFrameValidTest, RegisterFrameRejected)
{
    const uint64_t frame = makeMisoFrame(MISO_SENSOR_NEG512, false, SCH16T_RATE_X1, 0b00, false);
    EXPECT_FALSE(sch16tSensorFrameValid(frame, SCH16T_RATE_X1));
}

TEST(sch16tMisoFieldTest, DatasheetExampleDecode)
{
    EXPECT_EQ(0, sch16tMisoStatus(MISO_SENSOR_NEG512));
    EXPECT_EQ(0x001, sch16tMisoSa(MISO_SENSOR_NEG512));
    EXPECT_TRUE(sch16tMisoD(MISO_SENSOR_NEG512));
}

TEST(sch16tConfigValueTest, CtrlRateValue)
{
    EXPECT_EQ(((0b011 << 12) | (0b011 << 9) | (0b001 << 6) | (0b001 << 3) | 0b001),
              SCH16T_CTRL_RATE_VAL);
    EXPECT_EQ(0x3649, SCH16T_CTRL_RATE_VAL);
}

TEST(sch16tConfigValueTest, CtrlAcc12Value)
{
    EXPECT_EQ(((0b001 << 12) | (0b001 << 9) | (0b001 << 6) | (0b001 << 3) | 0b001),
              SCH16T_CTRL_ACC12_VAL);
    EXPECT_EQ(0x1249, SCH16T_CTRL_ACC12_VAL);
}

TEST(sch16tConfigValueTest, FilterAndUserIfValues)
{
    EXPECT_EQ(0x0DB, SCH16T_FILT_LPF3_ALL);
    EXPECT_EQ(0x202C, SCH16T_CTRL_USER_IF_VAL);
}

TEST(sch16tRegisterMapTest, AddressSanity)
{
    EXPECT_EQ(0x0A, SCH16T_RATE_X2);
    EXPECT_EQ(0x0D, SCH16T_ACC_X2);
    EXPECT_EQ(0x10, SCH16T_TEMP);
    EXPECT_EQ(0x35, SCH16T_CTRL_MODE);
    EXPECT_EQ(0x36, SCH16T_CTRL_RESET);
    EXPECT_EQ(0x3B, SCH16T_ASIC_ID);
    EXPECT_EQ(0x3C, SCH16T_COMP_ID);
}

TEST(sch16tSeqlockTest, StableGenerationCopies)
{
    const uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t dest[6] = {0};
    volatile uint32_t generation = 7;

    EXPECT_TRUE(sch16tSeqlockCopy(dest, src, sizeof(src), &generation, 7));
    EXPECT_EQ(0, memcmp(dest, src, sizeof(src)));
}

TEST(sch16tSeqlockTest, ChangedGenerationRejected)
{
    const uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t dest[6] = {0};
    volatile uint32_t generation = 8; // ISR bumped it (e.g.) from 7 to 8 before/during the copy

    EXPECT_FALSE(sch16tSeqlockCopy(dest, src, sizeof(src), &generation, 7));
}
