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

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "platform.h"

#if defined(USE_ACCGYRO_SCH16T) || defined(UNIT_TEST)

#include "drivers/accgyro/accgyro.h"
#include "drivers/bus.h"
#include "drivers/system.h"

#include "drivers/accgyro/accgyro_spi_sch16t.h"

// Pure protocol functions (no SPI dependency, host-testable)

#define SCH16T_CRC_DATA_MASK        0xFFFFFFFFFF00ULL
#define SCH16T_CRC_POLYNOMIAL       0x2FU
#define SCH16T_CRC_INITIAL          0xFFU
#define SCH16T_CRC_MSB_MASK         0x80U
#define SCH16T_FRAME_BIT_COUNT      48

#define SCH16T_ADDRESS_MASK         0x3FFU
#define SCH16T_MOSI_ADDRESS_SHIFT   38
#define SCH16T_MOSI_WRITE_BIT       (1ULL << 37)
#define SCH16T_MOSI_FRAME_TYPE_BIT  (1ULL << 35)

#define SCH16T_DATA_MASK            0xFFFFFULL
#define SCH16T_DATA_SHIFT           8
#define SCH16T_SENSOR_SIGN_BIT      0x80000
#define SCH16T_SENSOR_RANGE         0x100000

#define SCH16T_MISO_DATA_BIT        (1ULL << 47)
#define SCH16T_MISO_ADDRESS_SHIFT   37
#define SCH16T_MISO_STATUS_SHIFT    33
#define SCH16T_MISO_STATUS_MASK     0x03U
#define SCH16T_MISO_DCNT_SHIFT      29
#define SCH16T_MISO_DCNT_MASK       0x0FU

uint8_t sch16tCrc8(uint64_t frame48)
{
    const uint64_t data = frame48 & SCH16T_CRC_DATA_MASK;
    uint8_t crc = SCH16T_CRC_INITIAL;

    // The sensor clocks all 48 bits after replacing the received CRC byte with zero.
    for (int bit = SCH16T_FRAME_BIT_COUNT - 1; bit >= 0; bit--) {
        const uint8_t dataBit = (data >> bit) & 0x01U;
        crc = crc & SCH16T_CRC_MSB_MASK
            ? (uint8_t)((uint8_t)(crc << 1) ^ SCH16T_CRC_POLYNOMIAL) ^ dataBit
            : (uint8_t)(crc << 1) | dataBit;
    }

    return crc;
}

uint64_t sch16tFrameRead(uint16_t addr)
{
    const uint64_t frame = ((uint64_t)(addr & SCH16T_ADDRESS_MASK) << SCH16T_MOSI_ADDRESS_SHIFT)
        | SCH16T_MOSI_FRAME_TYPE_BIT;

    return frame | sch16tCrc8(frame);
}

uint64_t sch16tFrameWrite(uint16_t addr, uint32_t data20)
{
    const uint64_t frame = ((uint64_t)(addr & SCH16T_ADDRESS_MASK) << SCH16T_MOSI_ADDRESS_SHIFT)
        | SCH16T_MOSI_WRITE_BIT
        | SCH16T_MOSI_FRAME_TYPE_BIT
        | ((data20 & SCH16T_DATA_MASK) << SCH16T_DATA_SHIFT);

    return frame | sch16tCrc8(frame);
}

int32_t sch16tParseSensor20(uint64_t misoFrame)
{
    int32_t sensor = (int32_t)((misoFrame >> SCH16T_DATA_SHIFT) & SCH16T_DATA_MASK);
    if (sensor & SCH16T_SENSOR_SIGN_BIT) {
        sensor -= SCH16T_SENSOR_RANGE;
    }

    return sensor;
}

bool sch16tMisoFrameValid(uint64_t misoFrame)
{
    return sch16tCrc8(misoFrame) == (uint8_t)misoFrame
        && sch16tMisoStatus(misoFrame) == 0;
}

uint16_t sch16tMisoSa(uint64_t misoFrame)
{
    return (misoFrame >> SCH16T_MISO_ADDRESS_SHIFT) & SCH16T_ADDRESS_MASK;
}

uint8_t sch16tMisoStatus(uint64_t misoFrame)
{
    return (misoFrame >> SCH16T_MISO_STATUS_SHIFT) & SCH16T_MISO_STATUS_MASK;
}

uint8_t sch16tMisoDcnt(uint64_t misoFrame)
{
    return (misoFrame >> SCH16T_MISO_DCNT_SHIFT) & SCH16T_MISO_DCNT_MASK;
}

bool sch16tMisoD(uint64_t misoFrame)
{
    return (misoFrame & SCH16T_MISO_DATA_BIT) != 0;
}

#endif // USE_ACCGYRO_SCH16T || UNIT_TEST

#if defined(USE_ACCGYRO_SCH16T)

// TODO(T4): runtime driver

uint8_t sch16tSpiDetect(const extDevice_t *dev)
{
    UNUSED(dev);
    return 0;
}

bool sch16tSpiAccDetect(accDev_t *acc)
{
    UNUSED(acc);
    return false;
}

bool sch16tSpiGyroDetect(gyroDev_t *gyro)
{
    UNUSED(gyro);
    return false;
}

#endif // USE_ACCGYRO_SCH16T
