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

#endif // USE_ACCGYRO_SCH16T || UNIT_TEST

#if defined(USE_ACCGYRO_SCH16T) && !defined(UNIT_TEST)

#include "common/utils.h"

#include "drivers/io.h"
#include "drivers/resource.h"
#include "drivers/time.h"

#define SCH16T_DETECT_SPI_CLK_HZ       1000000
#define SCH16T_FRAME_SIZE              6
#define SCH16T_DETECT_FRAME_COUNT      3
#define SCH16T_BLOCKING_FRAME_COUNT    2
#define SCH16T_SAMPLE_FRAME_COUNT      7
#define SCH16T_SAMPLE_RESPONSE_COUNT   (SCH16T_SAMPLE_FRAME_COUNT - 1)
#define SCH16T_DMA_BUFFER_COUNT        2
#define SCH16T_RESET_PULSE_MS          2
#define SCH16T_STARTUP_DELAY_MS        250
#define SCH16T_TEMPERATURE_SCALE       100
#define SCH16T_EXTI_DETECT_THRESHOLD   1000  // mirrors GYRO_EXTI_DETECT_THRESHOLD (accgyro_mpu.c, file-local there)

static busSegment_t sch16tDmaSegments[SCH16T_DMA_BUFFER_COUNT][SCH16T_SAMPLE_FRAME_COUNT + 1];
STATIC_DMA_DATA_AUTO uint8_t sch16tDmaTx[SCH16T_SAMPLE_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDmaRx[SCH16T_DMA_BUFFER_COUNT][SCH16T_SAMPLE_FRAME_COUNT][SCH16T_FRAME_SIZE];
static volatile uint8_t sch16tRxActive;

STATIC_DMA_DATA_AUTO uint8_t sch16tWriteTx[SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tBlockingTx[SCH16T_BLOCKING_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tBlockingRx[SCH16T_BLOCKING_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDetectTx[SCH16T_DETECT_FRAME_COUNT][SCH16T_FRAME_SIZE];
STATIC_DMA_DATA_AUTO uint8_t sch16tDetectRx[SCH16T_DETECT_FRAME_COUNT][SCH16T_FRAME_SIZE];

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
        {.u.buffers = {sch16tWriteTx, NULL}, SCH16T_FRAME_SIZE, true, NULL},
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
        {.u.buffers = {sch16tBlockingTx[0], sch16tBlockingRx[0]}, SCH16T_FRAME_SIZE, true, NULL},
        {.u.buffers = {sch16tBlockingTx[1], sch16tBlockingRx[1]}, SCH16T_FRAME_SIZE, true, NULL},
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

static bool sch16tParseSample(gyroDev_t *gyro, uint8_t bufferIndex)
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

    // DCNT is per output (datasheet Tables 31/32), so it cannot validate cross-channel coherence.
    // DMA writes only the active set; the callback flips parity before publishing dataReady, leaving this set stable.
    for (unsigned index = 0; index < SCH16T_SAMPLE_RESPONSE_COUNT; index++) {
        responses[index] = sch16tFrameFromBytes(sch16tDmaRx[bufferIndex][index + 1]);
        if (!sch16tSensorFrameValid(responses[index], sourceAddresses[index])) {
            // PX4 counts CRC failures; Betaflight drops the complete sample.
            return false;
        }
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
    sch16tRxActive ^= 1;
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

    for (unsigned index = 0; index < SCH16T_SAMPLE_FRAME_COUNT; index++) {
        sch16tFrameToBytes(sch16tFrameRead(registers[index]), sch16tDmaTx[index]);
        for (unsigned bufferIndex = 0; bufferIndex < SCH16T_DMA_BUFFER_COUNT; bufferIndex++) {
            sch16tDmaSegments[bufferIndex][index].u.buffers.txData = sch16tDmaTx[index];
            sch16tDmaSegments[bufferIndex][index].u.buffers.rxData = sch16tDmaRx[bufferIndex][index];
            sch16tDmaSegments[bufferIndex][index].len = SCH16T_FRAME_SIZE;
            sch16tDmaSegments[bufferIndex][index].negateCS = true;
            sch16tDmaSegments[bufferIndex][index].callback = NULL;
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
    for (unsigned axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        acc->ADCRaw[axis] = sch16tAccRaw[axis];
    }

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

static void sch16tGyroInit(gyroDev_t *gyro)
{
    extDevice_t *dev = &gyro->dev;

    spiSetClkPhasePolarity(dev, true);
    spiSetClkDivisor(dev, spiCalculateDivider(SCH16T_MAX_SPI_CLK_HZ));

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
    sch16tWriteRegister(dev, SCH16T_CTRL_USER_IF, SCH16T_CTRL_USER_IF_VAL);
    sch16tWriteRegister(dev, SCH16T_CTRL_MODE, SCH16T_MODE_EN_SENSOR);
    delay(SCH16T_STARTUP_DELAY_MS);

    uint32_t statSum = 0;
    const bool statSumValid = sch16tReadRegisterBlocking(dev, SCH16T_STAT_SUM, &statSum);
    if (!statSumValid || statSum != SCH16T_STAT_SUM_OK) {
        // TODO(DEBUG_LEVEL): expose startup status; gyro init callbacks cannot report failure.
        (void)0;
    }

    uint32_t status;
    (void)sch16tReadRegisterBlocking(dev, SCH16T_STAT_SUM_SAT, &status);
    (void)sch16tReadRegisterBlocking(dev, SCH16T_STAT_COM, &status);

    sch16tWriteRegister(dev, SCH16T_CTRL_MODE, SCH16T_MODE_EOI);

    gyro->scale = SCH16T_GYRO_SCALE_DPS;
    gyro->mpuDividerDrops = 0;
    sch16tBuildDmaChain();
    mpuGyroInit(gyro);
}

static FAST_CODE bool sch16tGyroReadSPI(gyroDev_t *gyro)
{
    switch (gyro->gyroModeSPI) {
    case GYRO_EXTI_INIT:
        gyro->gyroDmaMaxDuration = 5;

        if (gyro->detectedEXTI > SCH16T_EXTI_DETECT_THRESHOLD) {
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
        } else {
            gyro->gyroModeSPI = GYRO_EXTI_NO_INT;
        }
        break;

    case GYRO_EXTI_INT:
    case GYRO_EXTI_NO_INT:
    {
        busSegment_t segments[SCH16T_SAMPLE_FRAME_COUNT + 1];
        memcpy(segments, sch16tDmaSegments[0], sizeof(segments));
        segments[SCH16T_SAMPLE_FRAME_COUNT - 1].callback = NULL;
        segments[SCH16T_SAMPLE_FRAME_COUNT].u.link.dev = NULL;
        segments[SCH16T_SAMPLE_FRAME_COUNT].u.link.segments = NULL;

        spiSequence(&gyro->dev, segments);
        spiWait(&gyro->dev);

        return sch16tParseSample(gyro, 0);
    }

    case GYRO_EXTI_INT_DMA:
        return sch16tParseSample(gyro, sch16tRxActive ^ 1);

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
            .callback = NULL,
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
