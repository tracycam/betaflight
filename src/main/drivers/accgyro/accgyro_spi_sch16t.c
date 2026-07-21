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
#include "drivers/time.h"

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
#define SCH16T_MISO_COMMAND_ERROR_BIT (1ULL << 35)
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

bool sch16tMisoCrcOk(uint64_t misoFrame)
{
    return sch16tCrc8(misoFrame) == (uint8_t)misoFrame;
}

bool sch16tRegisterFrameValid(uint64_t misoFrame, uint16_t sourceAddress)
{
    return sch16tMisoCrcOk(misoFrame)
        && (misoFrame & SCH16T_MISO_COMMAND_ERROR_BIT) == 0
        && !sch16tMisoD(misoFrame)
        && sch16tMisoSa(misoFrame) == sourceAddress;
}

bool sch16tSensorFrameValid(uint64_t misoFrame, uint16_t sourceAddress)
{
    const uint8_t status = sch16tMisoStatus(misoFrame);

    return sch16tMisoCrcOk(misoFrame)
        && (misoFrame & SCH16T_MISO_COMMAND_ERROR_BIT) == 0
        && sch16tMisoD(misoFrame)
        && sch16tMisoSa(misoFrame) == sourceAddress
        && (status == 0b00 || status == 0b10);
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

// Copy a sample snapshot; returns false when the completion ISR bumped the generation during the
// copy, meaning the source may have changed. Callers retry with a fresh generation and set.
bool sch16tSeqlockCopy(uint8_t *dest, const uint8_t *src, unsigned len, volatile uint32_t *generation, uint32_t generationBefore)
{
    memcpy(dest, src, len);
    return *generation == generationBefore;
}

STATIC_UNIT_TESTED busStatus_e sch16tFrameGapCallback(uintptr_t arg)
{
    (void)arg;
    delayMicroseconds(SCH16T_FRAME_GAP_US);
    return BUS_READY;
}

#ifndef UNIT_TEST
typedef struct {
    uint8_t dcnt[SCH16T_SENSOR_CHANNEL_COUNT];
    bool hasSample;
    uint32_t acceptedGeneration;
} sch16tFreshness_t;
#endif

STATIC_UNIT_TESTED void sch16tFreshnessReset(sch16tFreshness_t *state)
{
    memset(state, 0, sizeof(*state));
}

STATIC_UNIT_TESTED bool sch16tFreshnessAccept(sch16tFreshness_t *state, const uint8_t dcnt[SCH16T_SENSOR_CHANNEL_COUNT])
{
    if (state->hasSample) {
        for (unsigned index = 0; index < SCH16T_SENSOR_CHANNEL_COUNT; index++) {
            if (dcnt[index] == state->dcnt[index]) {
                return false;
            }
        }
    }

    memcpy(state->dcnt, dcnt, sizeof(state->dcnt));
    state->hasSample = true;
    state->acceptedGeneration++;

    return true;
}

STATIC_UNIT_TESTED bool sch16tSampleIsRecent(uint32_t nowUs, uint32_t completedAtUs)
{
    return (uint32_t)(nowUs - completedAtUs) < SCH16T_SAMPLE_TIMEOUT_US;
}

#endif // USE_ACCGYRO_SCH16T || UNIT_TEST

#if defined(USE_ACCGYRO_SCH16T) && !defined(UNIT_TEST)

#include "common/utils.h"

#include "drivers/io.h"
#include "drivers/resource.h"
#define SCH16T_DETECT_SPI_CLK_HZ       1000000
#define SCH16T_FRAME_SIZE              6
#define SCH16T_DETECT_FRAME_COUNT      3
#define SCH16T_BLOCKING_FRAME_COUNT    2
#define SCH16T_SAMPLE_FRAME_COUNT      7
#define SCH16T_SAMPLE_RESPONSE_COUNT   SCH16T_SENSOR_CHANNEL_COUNT
#define SCH16T_DMA_BUFFER_COUNT        2
#define SCH16T_STATUS_REGISTER_COUNT   10
#define SCH16T_CONFIG_REGISTER_COUNT   5
#define SCH16T_RESET_PULSE_MS          2
#define SCH16T_STARTUP_DELAY_MS        250
#define SCH16T_EOI_DELAY_MS            5
#define SCH16T_TEMPERATURE_SCALE       100

static busSegment_t sch16tDmaSegments[SCH16T_DMA_BUFFER_COUNT][SCH16T_SAMPLE_FRAME_COUNT + 1];
STATIC_DMA_DATA_AUTO uint8_t sch16tDmaTx[SCH16T_SAMPLE_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDmaRx[SCH16T_DMA_BUFFER_COUNT][SCH16T_SAMPLE_FRAME_COUNT][SCH16T_FRAME_SIZE];
static volatile uint8_t sch16tRxActive;
// Bumped by the completion callback after each parity flip; used by the snapshot seqlock.
static volatile uint32_t sch16tRxGeneration;
static volatile uint32_t sch16tRxCompletedAtUs[SCH16T_DMA_BUFFER_COUNT];
static uint32_t sch16tLastEvaluatedGeneration;
static uint32_t sch16tAccConsumedGeneration;
static sch16tFreshness_t sch16tFreshness;

STATIC_DMA_DATA_AUTO uint8_t sch16tWriteTx[SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tBlockingTx[SCH16T_BLOCKING_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tBlockingRx[SCH16T_BLOCKING_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDetectTx[SCH16T_DETECT_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDetectRx[SCH16T_DETECT_FRAME_COUNT][SCH16T_FRAME_SIZE];

// Dedicated blocking-chain buffers and descriptors; never referenced by the DMA chains.
STATIC_DMA_DATA_AUTO uint8_t sch16tBlockingChainRx[SCH16T_SAMPLE_FRAME_COUNT][SCH16T_FRAME_SIZE];
static busSegment_t sch16tBlockingSegments[SCH16T_SAMPLE_FRAME_COUNT + 1];

static int16_t sch16tAccRaw[XYZ_AXIS_COUNT];

static void sch16tFrameToBytes(uint64_t frame, uint8_t bytes[SCH16T_FRAME_SIZE])
{
    for (unsigned index = 0; index < SCH16T_FRAME_SIZE; index++) {
        bytes[index] = frame >> ((SCH16T_FRAME_SIZE - index - 1) * 8);
    }
}

static uint64_t sch16tFrameFromBytes(const uint8_t bytes[SCH16T_FRAME_SIZE])
{
    uint64_t frame = 0;

    for (unsigned index = 0; index < SCH16T_FRAME_SIZE; index++) {
        frame = (frame << 8) | bytes[index];
    }

    return frame;
}

static uint32_t sch16tData20(uint64_t frame)
{
    return (frame >> SCH16T_DATA_SHIFT) & SCH16T_DATA_MASK;
}

static void sch16tWriteRegister(const extDevice_t *dev, uint16_t addr, uint32_t data20)
{
    sch16tFrameToBytes(sch16tFrameWrite(addr, data20), sch16tWriteTx);

    busSegment_t segments[] = {
        {.u.buffers = {sch16tWriteTx, NULL}, SCH16T_FRAME_SIZE, true, sch16tFrameGapCallback},
        {.u.link = {NULL, NULL}, 0, true, NULL},
    };

    spiSequence(dev, segments);
    spiWait(dev);
}

static bool sch16tReadBlocking(const extDevice_t *dev, uint16_t addr, uint32_t *value20, bool sensorFrame)
{
    const uint64_t request = sch16tFrameRead(addr);

    for (unsigned index = 0; index < SCH16T_BLOCKING_FRAME_COUNT; index++) {
        sch16tFrameToBytes(request, sch16tBlockingTx[index]);
    }

    busSegment_t segments[] = {
        {.u.buffers = {sch16tBlockingTx[0], sch16tBlockingRx[0]}, SCH16T_FRAME_SIZE, true, sch16tFrameGapCallback},
        {.u.buffers = {sch16tBlockingTx[1], sch16tBlockingRx[1]}, SCH16T_FRAME_SIZE, true, sch16tFrameGapCallback},
        {.u.link = {NULL, NULL}, 0, true, NULL},
    };

    spiSequence(dev, segments);
    spiWait(dev);

    const uint64_t response = sch16tFrameFromBytes(sch16tBlockingRx[1]);
    const bool valid = sensorFrame
        ? sch16tSensorFrameValid(response, addr)
        : sch16tRegisterFrameValid(response, addr);
    if (!valid) {
        return false;
    }

    *value20 = sch16tData20(response);
    return true;
}

static bool sch16tReadRegisterBlocking(const extDevice_t *dev, uint16_t addr, uint32_t *value20)
{
    return sch16tReadBlocking(dev, addr, value20, false);
}

static bool sch16tReadSensorBlocking(const extDevice_t *dev, uint16_t addr, uint32_t *value20)
{
    return sch16tReadBlocking(dev, addr, value20, true);
}

static bool sch16tParseSample(gyroDev_t *gyro, const uint8_t frames[SCH16T_SAMPLE_RESPONSE_COUNT][SCH16T_FRAME_SIZE])
{
    static const uint16_t sourceAddresses[SCH16T_SAMPLE_RESPONSE_COUNT] = {
        SCH16T_RATE_X2,
        SCH16T_RATE_Y2,
        SCH16T_RATE_Z2,
        SCH16T_ACC_X2,
        SCH16T_ACC_Y2,
        SCH16T_ACC_Z2,
    };
    uint64_t responses[SCH16T_SAMPLE_RESPONSE_COUNT];
    uint8_t dcnt[SCH16T_SAMPLE_RESPONSE_COUNT];

    for (unsigned index = 0; index < SCH16T_SAMPLE_RESPONSE_COUNT; index++) {
        responses[index] = sch16tFrameFromBytes(frames[index]);
        if (!sch16tSensorFrameValid(responses[index], sourceAddresses[index])) {
            return false;
        }
        dcnt[index] = sch16tMisoDcnt(responses[index]);
    }

    if (!sch16tFreshnessAccept(&sch16tFreshness, dcnt)) {
        return false;
    }

    int16_t gyroRaw[XYZ_AXIS_COUNT];
    int16_t accRaw[XYZ_AXIS_COUNT];

    for (unsigned axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        gyroRaw[axis] = (int16_t)(sch16tParseSensor20(responses[axis]) >> 4);
        accRaw[axis] = (int16_t)(sch16tParseSensor20(responses[axis + XYZ_AXIS_COUNT]) >> 4);
    }

    for (unsigned axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        gyro->gyroADCRaw[axis] = gyroRaw[axis];
        sch16tAccRaw[axis] = accRaw[axis];
    }

    return true;
}

static busStatus_e sch16tDmaCallback(uintptr_t arg)
{
    sch16tFrameGapCallback(arg);
    sch16tRxCompletedAtUs[sch16tRxActive] = microsISR();
    __sync_synchronize();
    sch16tRxActive ^= 1;
    sch16tRxGeneration++;
    return mpuIntCallback(arg);
}

static void sch16tBuildDmaChain(void)
{
    static const uint16_t registers[SCH16T_SAMPLE_FRAME_COUNT] = {
        SCH16T_RATE_X2,
        SCH16T_RATE_Y2,
        SCH16T_RATE_Z2,
        SCH16T_ACC_X2,
        SCH16T_ACC_Y2,
        SCH16T_ACC_Z2,
        SCH16T_RATE_X2,
    };

    sch16tRxActive = 0;
    sch16tRxGeneration = 0;
    sch16tLastEvaluatedGeneration = 0;
    sch16tAccConsumedGeneration = 0;
    for (unsigned index = 0; index < SCH16T_DMA_BUFFER_COUNT; index++) {
        sch16tRxCompletedAtUs[index] = 0;
    }
    sch16tFreshnessReset(&sch16tFreshness);

    for (unsigned index = 0; index < SCH16T_SAMPLE_FRAME_COUNT; index++) {
        sch16tFrameToBytes(sch16tFrameRead(registers[index]), sch16tDmaTx[index]);
        for (unsigned bufferIndex = 0; bufferIndex < SCH16T_DMA_BUFFER_COUNT; bufferIndex++) {
            sch16tDmaSegments[bufferIndex][index].u.buffers.txData = sch16tDmaTx[index];
            sch16tDmaSegments[bufferIndex][index].u.buffers.rxData = sch16tDmaRx[bufferIndex][index];
            sch16tDmaSegments[bufferIndex][index].len = SCH16T_FRAME_SIZE;
            sch16tDmaSegments[bufferIndex][index].negateCS = true;
            sch16tDmaSegments[bufferIndex][index].callback = sch16tFrameGapCallback;
        }
    }

    for (unsigned bufferIndex = 0; bufferIndex < SCH16T_DMA_BUFFER_COUNT; bufferIndex++) {
        sch16tDmaSegments[bufferIndex][SCH16T_SAMPLE_FRAME_COUNT - 1].callback = sch16tDmaCallback;
        sch16tDmaSegments[bufferIndex][SCH16T_SAMPLE_FRAME_COUNT] = (busSegment_t){
            .u.link = {NULL, NULL},
            .len = 0,
            .negateCS = true,
            .callback = NULL,
        };
    }

    for (unsigned index = 0; index < SCH16T_SAMPLE_FRAME_COUNT; index++) {
        sch16tBlockingSegments[index].u.buffers.txData = sch16tDmaTx[index];
        sch16tBlockingSegments[index].u.buffers.rxData = sch16tBlockingChainRx[index];
        sch16tBlockingSegments[index].len = SCH16T_FRAME_SIZE;
        sch16tBlockingSegments[index].negateCS = true;
        sch16tBlockingSegments[index].callback = sch16tFrameGapCallback;
    }
    sch16tBlockingSegments[SCH16T_SAMPLE_FRAME_COUNT] = (busSegment_t){
        .u.link = {NULL, NULL},
        .len = 0,
        .negateCS = true,
        .callback = NULL,
    };
}

static bool sch16tGyroReadBlocking(gyroDev_t *gyro)
{
    // Dedicated descriptors and RX, never targeted by the DMA chains: this parse cannot race DMA.
    busSegment_t segments[SCH16T_SAMPLE_FRAME_COUNT + 1];
    memcpy(segments, sch16tBlockingSegments, sizeof(segments));

    spiSequence(&gyro->dev, segments);
    spiWait(&gyro->dev);

    return sch16tParseSample(gyro, sch16tBlockingChainRx + 1);
}

#ifdef USE_DMA
static void sch16tIntExtiHandler(extiCallbackRec_t *cb)
{
    gyroDev_t *gyro = container_of(cb, gyroDev_t, exti);
    const uint32_t nowCycles = getCycleCounter();
    const int32_t gyroLastPeriod = cmpTimeCycles(nowCycles, gyro->gyroLastEXTI);

    if ((gyro->gyroShortPeriod == 0) || (gyroLastPeriod < gyro->gyroShortPeriod)) {
        gyro->gyroSyncEXTI = gyro->gyroLastEXTI + gyro->gyroDmaMaxDuration;
    }
    gyro->gyroLastEXTI = nowCycles;

    if (gyro->gyroModeSPI == GYRO_EXTI_INT_DMA) {
        spiSequence(&gyro->dev, sch16tDmaSegments[sch16tRxActive]);
    }

    gyro->detectedEXTI++;
}
#endif

static void sch16tAccInit(accDev_t *acc)
{
    // The shared sensor is configured by gyro init before Betaflight initializes the accelerometer.
    acc->acc_1G = SCH16T_ACC_1G;
}

static FAST_CODE bool sch16tAccReadSPI(accDev_t *acc)
{
    const uint32_t generation = sch16tFreshness.acceptedGeneration;
    if (generation == sch16tAccConsumedGeneration) {
        return false;
    }

    for (unsigned axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        acc->ADCRaw[axis] = sch16tAccRaw[axis];
    }
    sch16tAccConsumedGeneration = generation;

    return true;
}

static bool sch16tTemperatureRead(gyroDev_t *gyro, int16_t *temperature)
{
    uint32_t value20;
    if (!sch16tReadSensorBlocking(&gyro->dev, SCH16T_TEMP, &value20)) {
        return false;
    }

    *temperature = (int16_t)(value20 >> 4) / SCH16T_TEMPERATURE_SCALE;
    return true;
}

static bool sch16tReadStatusRegisters(const extDevice_t *dev, uint32_t values[SCH16T_STATUS_REGISTER_COUNT])
{
    static const uint16_t registers[SCH16T_STATUS_REGISTER_COUNT] = {
        SCH16T_STAT_SUM,
        SCH16T_STAT_SUM_SAT,
        SCH16T_STAT_COM,
        SCH16T_STAT_RATE_COM,
        SCH16T_STAT_RATE_X,
        SCH16T_STAT_RATE_Y,
        SCH16T_STAT_RATE_Z,
        SCH16T_STAT_ACC_X,
        SCH16T_STAT_ACC_Y,
        SCH16T_STAT_ACC_Z,
    };
    bool valid = true;

    for (unsigned index = 0; index < SCH16T_STATUS_REGISTER_COUNT; index++) {
        values[index] = 0;
        if (!sch16tReadRegisterBlocking(dev, registers[index], &values[index])) {
            valid = false;
        }
    }

    return valid;
}

static bool sch16tConfigRegistersValid(const extDevice_t *dev)
{
    static const uint16_t registers[SCH16T_CONFIG_REGISTER_COUNT] = {
        SCH16T_CTRL_FILT_RATE,
        SCH16T_CTRL_FILT_ACC12,
        SCH16T_CTRL_RATE,
        SCH16T_CTRL_ACC12,
        SCH16T_CTRL_USER_IF,
    };
    static const uint32_t expectedValues[SCH16T_CONFIG_REGISTER_COUNT] = {
        SCH16T_FILT_LPF3_ALL,
        SCH16T_FILT_LPF3_ALL,
        SCH16T_CTRL_RATE_VAL,
        SCH16T_CTRL_ACC12_VAL,
        SCH16T_CTRL_USER_IF_VAL,
    };
    bool valid = true;

    for (unsigned index = 0; index < SCH16T_CONFIG_REGISTER_COUNT; index++) {
        uint32_t value = 0;
        if (!sch16tReadRegisterBlocking(dev, registers[index], &value) || value != expectedValues[index]) {
            valid = false;
        }
    }

    return valid;
}

static bool sch16tInitOnce(gyroDev_t *gyro)
{
    extDevice_t *dev = &gyro->dev;

#if defined(GYRO_1_RST_PIN)
    const IO_t resetPin = IOGetByTag(IO_TAG(GYRO_1_RST_PIN));
    IOInit(resetPin, OWNER_SYSTEM, 0);
    IOConfigGPIO(resetPin, IOCFG_OUT_PP);
    IOLo(resetPin);
    delay(SCH16T_RESET_PULSE_MS);
    IOHi(resetPin);
#else
    sch16tWriteRegister(dev, SCH16T_CTRL_RESET, SCH16T_RESET_SOFT);
#endif
    delay(SCH16T_STARTUP_DELAY_MS);

    sch16tWriteRegister(dev, SCH16T_CTRL_FILT_RATE, SCH16T_FILT_LPF3_ALL);
    sch16tWriteRegister(dev, SCH16T_CTRL_FILT_ACC12, SCH16T_FILT_LPF3_ALL);
    sch16tWriteRegister(dev, SCH16T_CTRL_RATE, SCH16T_CTRL_RATE_VAL);
    sch16tWriteRegister(dev, SCH16T_CTRL_ACC12, SCH16T_CTRL_ACC12_VAL);
    // ACC_XYZ3 is interpolated, not part of the decimated DRY set, and CTRL_ACC3 only carries
    // DYN_ACC3[2:0] (datasheet Table 67): leave it at the reset default.
    sch16tWriteRegister(dev, SCH16T_CTRL_USER_IF, SCH16T_CTRL_USER_IF_VAL);
    sch16tWriteRegister(dev, SCH16T_CTRL_MODE, SCH16T_MODE_EN_SENSOR);
    delay(SCH16T_STARTUP_DELAY_MS);

    uint32_t statusValues[SCH16T_STATUS_REGISTER_COUNT];
    // Clear startup flags before EOI, then validate the second of two post-EOI status passes.
    (void)sch16tReadStatusRegisters(dev, statusValues);

    sch16tWriteRegister(dev, SCH16T_CTRL_MODE, SCH16T_MODE_EOI);
    delay(SCH16T_EOI_DELAY_MS);

    (void)sch16tReadStatusRegisters(dev, statusValues);
    const bool statusFramesValid = sch16tReadStatusRegisters(dev, statusValues);
    bool statusValuesValid = true;
    for (unsigned index = 0; index < SCH16T_STATUS_REGISTER_COUNT; index++) {
        if (statusValues[index] != SCH16T_STAT_SUM_OK) {
            statusValuesValid = false;
        }
    }

    const bool configValid = sch16tConfigRegistersValid(dev);
    return statusFramesValid && statusValuesValid && configValid;
}

static void sch16tGyroInit(gyroDev_t *gyro)
{
    extDevice_t *dev = &gyro->dev;

    spiSetClkPhasePolarity(dev, true);
    spiSetClkDivisor(dev, spiCalculateDivider(SCH16T_MAX_SPI_CLK_HZ));

    bool initialized = false;
    for (unsigned attempt = 0; attempt < SCH16T_INIT_ATTEMPTS; attempt++) {
        if (sch16tInitOnce(gyro)) {
            initialized = true;
            break;
        }
    }
    if (!initialized) {
        failureMode(FAILURE_GYRO_INIT_FAILED);
        return;
    }

    gyro->scale = SCH16T_GYRO_SCALE_DPS;
    gyro->mpuDividerDrops = 0;
    sch16tBuildDmaChain();
    mpuGyroInit(gyro);
    if (!sch16tGyroReadBlocking(gyro)) {
        failureMode(FAILURE_GYRO_INIT_FAILED);
    }
}

static FAST_CODE bool sch16tGyroReadSPI(gyroDev_t *gyro)
{
    switch (gyro->gyroModeSPI) {
    case GYRO_EXTI_INIT:
        gyro->gyroDmaMaxDuration = 5;

        if (gyro->mpuIntExtiTag == IO_TAG_NONE) {
            // No EXTI/DRY line on this target: poll the sensor from the gyro task.
            gyro->gyroModeSPI = GYRO_EXTI_NO_INT;
            return sch16tGyroReadBlocking(gyro);
        }

#ifdef USE_DMA
        if (spiUseDMA(&gyro->dev)) {
            gyro->dev.callbackArg = (uintptr_t)gyro;

            // The common two-segment MPU chain cannot hold seven SafeSPI frames.
            EXTIHandlerInit(&gyro->exti, sch16tIntExtiHandler);
            gyro->gyroModeSPI = GYRO_EXTI_INT_DMA;
        } else
#endif
        {
            gyro->gyroModeSPI = GYRO_EXTI_INT;
        }

        return sch16tGyroReadBlocking(gyro);

    case GYRO_EXTI_INT:
    case GYRO_EXTI_NO_INT:
        return sch16tGyroReadBlocking(gyro);

    case GYRO_EXTI_INT_DMA:
        if (sch16tRxGeneration == sch16tLastEvaluatedGeneration) {
            if (!spiIsBusy(&gyro->dev)) {
                return sch16tGyroReadBlocking(gyro);
            }
            return false;
        }
        {
            uint8_t snapshot[SCH16T_SAMPLE_RESPONSE_COUNT][SCH16T_FRAME_SIZE];
            unsigned set;
            uint32_t generation;
            uint32_t completedAtUs;
            do {
                generation = sch16tRxGeneration;
                set = sch16tRxActive ^ 1;
                completedAtUs = sch16tRxCompletedAtUs[set];
            } while (!sch16tSeqlockCopy(&snapshot[0][0], &sch16tDmaRx[set][1][0], sizeof(snapshot), &sch16tRxGeneration, generation));
            sch16tLastEvaluatedGeneration = generation;
            gyro->dataReady = false;

            const uint32_t nowUs = micros();
            if (!sch16tSampleIsRecent(nowUs, completedAtUs)) {
                return false;
            }
            return sch16tParseSample(gyro, snapshot);
        }

    default:
        break;
    }

    return true;
}

uint8_t sch16tSpiDetect(const extDevice_t *dev)
{
    spiSetClkPhasePolarity(dev, true);
    spiSetClkDivisor(dev, spiCalculateDivider(SCH16T_DETECT_SPI_CLK_HZ));

    const uint16_t registers[SCH16T_DETECT_FRAME_COUNT] = {
        SCH16T_COMP_ID,
        SCH16T_ASIC_ID,
        SCH16T_COMP_ID,
    };

    busSegment_t segments[SCH16T_DETECT_FRAME_COUNT + 1];
    for (unsigned index = 0; index < SCH16T_DETECT_FRAME_COUNT; index++) {
        sch16tFrameToBytes(sch16tFrameRead(registers[index]), sch16tDetectTx[index]);
        segments[index] = (busSegment_t){
            .u.buffers = {sch16tDetectTx[index], sch16tDetectRx[index]},
            .len = SCH16T_FRAME_SIZE,
            .negateCS = true,
            .callback = sch16tFrameGapCallback,
        };
    }
    segments[SCH16T_DETECT_FRAME_COUNT] = (busSegment_t){
        .u.link = {NULL, NULL},
        .len = 0,
        .negateCS = true,
        .callback = NULL,
    };

    spiSequence(dev, segments);
    spiWait(dev);

    const uint64_t compIdFrame = sch16tFrameFromBytes(sch16tDetectRx[1]);
    const uint64_t asicIdFrame = sch16tFrameFromBytes(sch16tDetectRx[2]);
    if (!sch16tRegisterFrameValid(compIdFrame, SCH16T_COMP_ID)
        || !sch16tRegisterFrameValid(asicIdFrame, SCH16T_ASIC_ID)) {
        return MPU_NONE;
    }

    const uint32_t compId = sch16tData20(compIdFrame);
    const uint32_t asicId = sch16tData20(asicIdFrame);

    return asicId == SCH16T_ASIC_ID_K10 && compId == SCH16T_COMP_ID_K10
        ? SCH16T_SPI
        : MPU_NONE;
}

bool sch16tSpiAccDetect(accDev_t *acc)
{
    if (acc->mpuDetectionResult.sensor != SCH16T_SPI) {
        return false;
    }

    acc->initFn = sch16tAccInit;
    acc->readFn = sch16tAccReadSPI;
    return true;
}

bool sch16tSpiGyroDetect(gyroDev_t *gyro)
{
    if (gyro->mpuDetectionResult.sensor != SCH16T_SPI) {
        return false;
    }

    gyro->initFn = sch16tGyroInit;
    gyro->readFn = sch16tGyroReadSPI;
    gyro->temperatureFn = sch16tTemperatureRead;
    return true;
}

#endif // USE_ACCGYRO_SCH16T && !UNIT_TEST
