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
// TODO(T2): implement CRC8/frame logic against golden vectors

uint8_t sch16tCrc8(uint64_t frame48)
{
    UNUSED(frame48);
    return 0;
}

uint64_t sch16tFrameRead(uint16_t addr)
{
    UNUSED(addr);
    return 0;
}

uint64_t sch16tFrameWrite(uint16_t addr, uint32_t data20)
{
    UNUSED(addr);
    UNUSED(data20);
    return 0;
}

int32_t sch16tParseSensor20(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return 0;
}

bool sch16tMisoFrameValid(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return false;
}

uint16_t sch16tMisoSa(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return 0;
}

uint8_t sch16tMisoStatus(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return 0;
}

uint8_t sch16tMisoDcnt(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return 0;
}

bool sch16tMisoD(uint64_t misoFrame)
{
    UNUSED(misoFrame);
    return false;
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
