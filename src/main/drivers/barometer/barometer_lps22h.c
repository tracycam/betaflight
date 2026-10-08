#include <stdbool.h>
#include <stdint.h>

#include "platform.h"

#if defined(USE_BARO) && (defined(USE_BARO_LPS22H) || defined(USE_BARO_SPI_LPS22H))

#include "common/utils.h"

#include "drivers/barometer/barometer.h"
#include "drivers/bus.h"
#include "drivers/bus_i2c.h"
#include "drivers/bus_i2c_busdev.h"
#include "drivers/bus_spi.h"
#include "drivers/io.h"
#include "drivers/time.h"

#include "barometer_lps22h.h"

#define LPS22H_MAX_SPI_CLK_HZ 10000000

// Macros to encode/decode multi-bit values
#define LPS22H_ENCODE_BITS(val, mask, shift)   (((val) << (shift)) & (mask))

#define LPS22H_INTERRUPT_CFG               0x0B
#define LPS22H_THS_P_L                     0x0C
#define LPS22H_THS_P_H                     0x0D

#define LPS22H_WHO_AM_I                    0x0F
#define LPS22HB_CHIP_ID                    0xB1
#define LPS22HH_CHIP_ID                    0xB3

#define LPS22H_CTRL_REG1                   0x10
#define LPS22H_CTRL_REG1_ODR_MASK          0x70
#define LPS22H_CTRL_REG1_ODR_SHIFT         4
#define LPS22H_CTRL_REG1_ODR_POWER_DOWN    0
#define LPS22H_CTRL_REG1_BDU               0x02

#define LPS22H_CTRL_REG2                   0x11
#define LPS22H_CTRL_REG2_IF_ADD_INC        0x10
#define LPS22H_CTRL_REG2_SWRESET           0x04
#define LPS22H_CTRL_REG2_ONE_SHOT          0x01

#define LPS22H_STATUS                      0x27
#define LPS22H_STATUS_P_DA                 0x01

#define LPS22H_PRESSURE_OUT_XL             0x28
#define LPS22H_PRESSURE_OUT_L              0x29
#define LPS22H_PRESSURE_OUT_H              0x2A
#define LPS22H_TEMP_OUT_L                  0x2B
#define LPS22H_TEMP_OUT_H                  0x2C

#define LPS22H_I2C_ADDR                    0x5D

#define LPS22H_DATA_FRAME_SIZE (LPS22H_TEMP_OUT_H - LPS22H_PRESSURE_OUT_XL + 1)

static uint8_t lps22hChipId;
static int32_t lps22hRawPressure;
static int32_t lps22hRawTemperature;
static DMA_DATA_ZERO_INIT uint8_t lps22hBuffer[LPS22H_DATA_FRAME_SIZE];

static bool lps22hStartUT(baroDev_t *baro);
static bool lps22hReadUT(baroDev_t *baro);
static bool lps22hGetUT(baroDev_t *baro);
static bool lps22hStartUP(baroDev_t *baro);
static bool lps22hReadUP(baroDev_t *baro);
static bool lps22hGetUP(baroDev_t *baro);
static void lps22hCalculate(int32_t *pressure, int32_t *temperature);

static void lps22hBusInit(const extDevice_t *dev)
{
#ifdef USE_BARO_SPI_LPS22H
    if (dev->bus->busType == BUS_TYPE_SPI) {
        IOInit(dev->busType_u.spi.csnPin, OWNER_BARO_CS, 0);
        IOConfigGPIO(dev->busType_u.spi.csnPin, IOCFG_OUT_PP);
        IOHi(dev->busType_u.spi.csnPin);
        spiSetClkDivisor(dev, spiCalculateDivider(LPS22H_MAX_SPI_CLK_HZ));
    }
#else
    UNUSED(dev);
#endif
}

static void lps22hBusDeinit(const extDevice_t *dev)
{
#ifdef USE_BARO_SPI_LPS22H
    if (dev->bus->busType == BUS_TYPE_SPI) {
        ioPreinitByIO(dev->busType_u.spi.csnPin, IOCFG_IPU, PREINIT_PIN_STATE_HIGH);
    }
#else
    UNUSED(dev);
#endif
}

bool lps22hDetect(baroDev_t *baro)
{
    delay(20);

    extDevice_t *dev = &baro->dev;
    bool defaultAddressApplied = false;

    lps22hBusInit(dev);

    if ((dev->bus->busType == BUS_TYPE_I2C) && dev->busType_u.i2c.address == 0) {
        dev->busType_u.i2c.address = LPS22H_I2C_ADDR;
        defaultAddressApplied = true;
    }

    if (!busReadRegisterBuffer(dev, LPS22H_WHO_AM_I, &lps22hChipId, 1)) {
        lps22hBusDeinit(dev);
        if (defaultAddressApplied) {
            dev->busType_u.i2c.address = 0;
        }
        return false;
    }

    if (lps22hChipId != LPS22HB_CHIP_ID && lps22hChipId != LPS22HH_CHIP_ID) {
        lps22hBusDeinit(dev);
        if (defaultAddressApplied) {
            dev->busType_u.i2c.address = 0;
        }
        return false;
    }

    busDeviceRegister(dev);

    busWriteRegister(dev, LPS22H_CTRL_REG2, LPS22H_CTRL_REG2_SWRESET);
    busWriteRegister(dev, LPS22H_CTRL_REG1, LPS22H_ENCODE_BITS(LPS22H_CTRL_REG1_ODR_POWER_DOWN, LPS22H_CTRL_REG1_ODR_MASK, LPS22H_CTRL_REG1_ODR_SHIFT));

    baro->combined_read = true;
    baro->ut_delay = 0;
    baro->start_ut = lps22hStartUT;
    baro->read_ut = lps22hReadUT;
    baro->get_ut = lps22hGetUT;
    baro->start_up = lps22hStartUP;
    baro->read_up = lps22hReadUP;
    baro->get_up = lps22hGetUP;
    baro->up_delay = 10000;
    baro->calculate = lps22hCalculate;

    return true;
}

static bool lps22hStartUT(baroDev_t *baro)
{
    UNUSED(baro);
    return true;
}

static bool lps22hReadUT(baroDev_t *baro)
{
    UNUSED(baro);
    return true;
}

static bool lps22hGetUT(baroDev_t *baro)
{
    UNUSED(baro);
    return true;
}

static bool lps22hStartUP(baroDev_t *baro)
{
    return busWriteRegister(&baro->dev, LPS22H_CTRL_REG2, LPS22H_CTRL_REG2_ONE_SHOT | LPS22H_CTRL_REG2_IF_ADD_INC);
}

static bool lps22hReadUP(baroDev_t *baro)
{
    if (busBusy(&baro->dev, NULL)) {
        return false;
    }

    uint8_t status;
    if (!busReadRegisterBuffer(&baro->dev, LPS22H_STATUS, &status, 1)) {
        return false;
    }

    if (!(status & LPS22H_STATUS_P_DA)) {
        return false;
    }

    return busReadRegisterBufferStart(&baro->dev, LPS22H_PRESSURE_OUT_XL, lps22hBuffer, LPS22H_DATA_FRAME_SIZE);
}

static bool lps22hGetUP(baroDev_t *baro)
{
    if (busBusy(&baro->dev, NULL)) {
        return false;
    }

    lps22hRawPressure = (int32_t)(lps22hBuffer[0] | (lps22hBuffer[1] << 8) | (lps22hBuffer[2] << 16));
    if (lps22hRawPressure & 0x800000) {
        lps22hRawPressure |= 0xFF000000;
    }

    lps22hRawTemperature = (int32_t)(lps22hBuffer[3] | (lps22hBuffer[4] << 8));
    if (lps22hRawTemperature & 0x8000) {
        lps22hRawTemperature |= 0xFFFF0000;
    }

    return true;
}

static int32_t lps22hCompensateTemperature(int32_t rawTemperature)
{
    return rawTemperature;
}

static uint32_t lps22hCompensatePressure(int32_t rawPressure)
{
    return (uint32_t)((rawPressure * 100.0f) / 16.0f);
}

static void lps22hCalculate(int32_t *pressure, int32_t *temperature)
{
    if (temperature) {
        *temperature = lps22hCompensateTemperature(lps22hRawTemperature);
    }

    if (pressure) {
        const uint32_t compensated = lps22hCompensatePressure(lps22hRawPressure);
        *pressure = (int32_t)(compensated / 256U);
    }
}

#endif
