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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "drivers/accgyro/accgyro.h"
#include "drivers/bus_spi.h"

// Murata SCH16T-K10 6-DOF gyro/accelerometer
//
// SPI protocol uses 48-bit frames.
//
// MOSI frame layout:
//   [47:38] TA    - target address (10-bit register address)
//   [37]    RW    - 1 = write, 0 = read
//   [36]    0     - reserved
//   [35]    FT    - frame type, always 1
//   [34:28] 0     - reserved
//   [27:8]  DATAI - 20-bit write data (ignored on read)
//   [7:0]   CRC8  - CRC-8 (poly 0x2F, init 0xFF, MSB-first) over bits 47..8
//
// MISO frame layout:
//   [47]    D     - 1 = RATE/ACC/TEMP sensor data, 0 = other register data
//   [46:37] SA    - source address, echoes the requested register address
//   [36]    IDS   - redundant common-error indication; S carries the accurate status
//   [35]    CE    - command error; 1 indicates an invalid or desynchronized request
//   [34:33] S     - 00 normal, 01 error, 10 valid saturated data, 11 initialization
//                    S is undefined before EOI and is always 00 on write responses
//   [32:29] DCNT  - per-output counter for decimated RATE_XYZ2/ACC_XYZ2 sensor data;
//                    unused on register frames and not comparable across channels
//   [28]    0     - reserved
//   [27:8]  DATA  - 20-bit SENSOR data (two's complement) or register INFO data
//   [7:0]   CRC8  - CRC-8 (poly 0x2F, init 0xFF, MSB-first) over bits 47..8

// Register addresses
#define SCH16T_RATE_X1          0x01
#define SCH16T_RATE_Y1          0x02
#define SCH16T_RATE_Z1          0x03
#define SCH16T_ACC_X1           0x04
#define SCH16T_ACC_Y1           0x05
#define SCH16T_ACC_Z1           0x06
#define SCH16T_ACC_X3           0x07
#define SCH16T_ACC_Y3           0x08
#define SCH16T_ACC_Z3           0x09
#define SCH16T_RATE_X2          0x0A
#define SCH16T_RATE_Y2          0x0B
#define SCH16T_RATE_Z2          0x0C
#define SCH16T_ACC_X2           0x0D
#define SCH16T_ACC_Y2           0x0E
#define SCH16T_ACC_Z2           0x0F
#define SCH16T_TEMP             0x10
#define SCH16T_STAT_SUM         0x14
#define SCH16T_STAT_SUM_SAT     0x15
#define SCH16T_STAT_COM         0x16
#define SCH16T_CTRL_FILT_RATE   0x25
#define SCH16T_CTRL_FILT_ACC12  0x26
#define SCH16T_CTRL_FILT_ACC3   0x27
#define SCH16T_CTRL_RATE        0x28
#define SCH16T_CTRL_ACC12       0x29
#define SCH16T_CTRL_ACC3        0x2A
#define SCH16T_CTRL_USER_IF     0x33
#define SCH16T_CTRL_ST          0x34
#define SCH16T_CTRL_MODE        0x35
#define SCH16T_CTRL_RESET       0x36
#define SCH16T_SYS_TEST         0x37
#define SCH16T_ASIC_ID          0x3B
#define SCH16T_COMP_ID          0x3C
#define SCH16T_SN_ID1           0x3D
#define SCH16T_SN_ID2           0x3E
#define SCH16T_SN_ID3           0x3F

// Device identity
#define SCH16T_ASIC_ID_K10      0x21
#define SCH16T_COMP_ID_K10      0x21

// CTRL_FILT_* values: LPF3 = 0b011 for all axes (3 | (3 << 3) | (3 << 6))
#define SCH16T_FILT_LPF3_ALL    0x0DB

// CTRL_USER_IF value: reset default 0x200C | DRY_DRV_EN (bit 5, 0x20)
#define SCH16T_CTRL_USER_IF_VAL 0x202C

// DYN/DEC field encodings for CTRL_RATE / CTRL_ACC12
#define SCH16T_DYN1             0b001
#define SCH16T_DYN3             0b011
#define SCH16T_DEC2             0b001

// CTRL_RATE / CTRL_ACC12 layout:
//   [14:12] DYN_XYZ1, [11:9] DYN_XYZ2, [8:6] DEC_Z2, [5:3] DEC_Y2, [2:0] DEC_X2
#define SCH16T_CTRL_RATE_VAL \
    ((SCH16T_DYN3 << 12) | (SCH16T_DYN3 << 9) | (SCH16T_DEC2 << 6) | (SCH16T_DEC2 << 3) | SCH16T_DEC2)
#define SCH16T_CTRL_ACC12_VAL \
    ((SCH16T_DYN1 << 12) | (SCH16T_DYN1 << 9) | (SCH16T_DEC2 << 6) | (SCH16T_DEC2 << 3) | SCH16T_DEC2)

// CTRL_RESET / CTRL_MODE / status values
#define SCH16T_RESET_SOFT       0xA
#define SCH16T_MODE_EN_SENSOR   0x1
#define SCH16T_MODE_EOI         0x3
#define SCH16T_STAT_SUM_OK      0xFFFF

// 10.5 MHz max SPI frequency (MISO_HI_SPD = 0 default limit)
#define SCH16T_MAX_SPI_CLK_HZ   10500000

// Sensitivity
// DYN3 gyro range: 200 LSB/(dps) at 20-bit resolution; after >>4 -> 12.5 LSB/(dps)
#define SCH16T_GYRO_SCALE_DPS   0.08f
// ACC12: 200 LSB/(m/s^2) * 9.80665 m/s^2 = 1961 LSB/g
#define SCH16T_ACC_1G           1961

// Pure protocol functions (host-testable, no SPI types in signatures)

// CRC-8 over bits 47..8 of a 48-bit frame (poly 0x2F, init 0xFF, MSB-first)
uint8_t sch16tCrc8(uint64_t frame48);

// Build a 48-bit MOSI frame
uint64_t sch16tFrameRead(uint16_t addr);
uint64_t sch16tFrameWrite(uint16_t addr, uint32_t data20);

// Extract the sign-extended 20-bit SENSOR field (bits 27..8) from a MISO frame
int32_t sch16tParseSensor20(uint64_t misoFrame);

// Validate only the MISO CRC8 field
bool sch16tMisoCrcOk(uint64_t misoFrame);

// Validate an other-data register response; S and DCNT are not meaningful before EOI/on this frame class
bool sch16tRegisterFrameValid(uint64_t misoFrame, uint16_t sourceAddress);

// Validate a sensor-data response; normal and saturated samples are both valid
bool sch16tSensorFrameValid(uint64_t misoFrame, uint16_t sourceAddress);

// MISO field helpers
uint16_t sch16tMisoSa(uint64_t misoFrame);
uint8_t sch16tMisoStatus(uint64_t misoFrame);
uint8_t sch16tMisoDcnt(uint64_t misoFrame);
bool sch16tMisoD(uint64_t misoFrame);

// Driver API
uint8_t sch16tSpiDetect(const extDevice_t *dev);
bool sch16tSpiAccDetect(accDev_t *acc);
bool sch16tSpiGyroDetect(gyroDev_t *gyro);
