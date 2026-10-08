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
#include <string.h>

extern "C" {
    #include "platform.h"

    #include "common/spec.h"

    #include "drivers/motor.h"
    #include "flight/mixer.h"
    #include "pg/motor.h"
    #include "pg/pg.h"
    #include "pg/pg_ids.h"

    PG_REGISTER(mixerConfig_t, mixerConfig, PG_MIXER_CONFIG, 0);
    PG_REGISTER(motorConfig_t, motorConfig, PG_MOTOR_CONFIG, 0);

    // Mock for the bidirectional-dshot gate, controlled per-test.
    bool unitTestBidirDshot = true;
    bool isMotorProtocolBidirDshot(void) { return unitTestBidirDshot; }
}

#include "gtest/gtest.h"

// Expected preset table (mirrors specArray[] in spec.c).
struct ExpectedSpec {
    SpecType type;
    bool rpm_limit;
    uint16_t rpm_limit_p;
    uint16_t rpm_limit_i;
    uint16_t rpm_limit_d;
    uint16_t rpm_limit_value;
    uint8_t motorPoleCount;
    uint16_t kv;
};

static const ExpectedSpec expectedSpecs[SPEC_COUNT] = {
    { SPEC_FREEDOM,  true, 25, 10, 8, 18000, 14, 1960 },
    { SPEC_MGP_PRO,  true, 25, 10, 8, 13000, 14, 1300 },
    { SPEC_MAYHEM,   true, 25, 10, 8, 24000, 14, 1960 },
    { SPEC_TT,       true, 25, 10, 8, 30000, 12, 4533 },
    { SPEC_LLIGUETA, true, 25, 10, 8, 17000, 14, 1980 },
};

class SpecTest : public ::testing::Test {
protected:
    void SetUp() override {
        unitTestBidirDshot = true;
        memset(mixerConfigMutable(), 0, sizeof(mixerConfig_t));
        memset(motorConfigMutable(), 0, sizeof(motorConfig_t));
    }
};

// 1. Round-trip for ALL 5 presets.
TEST_F(SpecTest, RoundTripAllPresets)
{
    unitTestBidirDshot = true;
    for (int s = 0; s < SPEC_COUNT; s++) {
        setSpec((SpecType)s);
        EXPECT_EQ((SpecType)s, getCurrentSpec()) << "failed round-trip for spec index " << s;
    }
}

// 2. setSpec writes the correct values for representative presets.
TEST_F(SpecTest, SetSpecWritesCorrectValues)
{
    for (int s = 0; s < SPEC_COUNT; s++) {
        const ExpectedSpec &e = expectedSpecs[s];
        setSpec((SpecType)s);

        EXPECT_EQ(e.rpm_limit, mixerConfig()->rpm_limit) << "spec index " << s;
        EXPECT_EQ(e.rpm_limit_p, mixerConfig()->rpm_limit_p) << "spec index " << s;
        EXPECT_EQ(e.rpm_limit_i, mixerConfig()->rpm_limit_i) << "spec index " << s;
        EXPECT_EQ(e.rpm_limit_d, mixerConfig()->rpm_limit_d) << "spec index " << s;
        EXPECT_EQ(e.rpm_limit_value, mixerConfig()->rpm_limit_value) << "spec index " << s;
        EXPECT_EQ(e.motorPoleCount, motorConfig()->motorPoleCount) << "spec index " << s;
        EXPECT_EQ(e.kv, motorConfig()->kv) << "spec index " << s;
    }

    // Explicit checks for SPEC_TT (unique poles=12, kv=4533).
    setSpec(SPEC_TT);
    EXPECT_EQ(30000, mixerConfig()->rpm_limit_value);
    EXPECT_EQ(12, motorConfig()->motorPoleCount);
    EXPECT_EQ(4533, motorConfig()->kv);
    EXPECT_EQ(25, mixerConfig()->rpm_limit_p);
    EXPECT_EQ(10, mixerConfig()->rpm_limit_i);
    EXPECT_EQ(8, mixerConfig()->rpm_limit_d);

    // Explicit checks for SPEC_FREEDOM.
    setSpec(SPEC_FREEDOM);
    EXPECT_EQ(18000, mixerConfig()->rpm_limit_value);
    EXPECT_EQ(14, motorConfig()->motorPoleCount);
    EXPECT_EQ(1960, motorConfig()->kv);
    EXPECT_EQ(25, mixerConfig()->rpm_limit_p);
    EXPECT_EQ(10, mixerConfig()->rpm_limit_i);
    EXPECT_EQ(8, mixerConfig()->rpm_limit_d);
}

// 3. checkSpec true/false.
TEST_F(SpecTest, CheckSpecTrueFalse)
{
    unitTestBidirDshot = true;
    setSpec(SPEC_FREEDOM);
    EXPECT_TRUE(checkSpec(SPEC_FREEDOM));
    EXPECT_FALSE(checkSpec(SPEC_MAYHEM));
}

// 4. No-match returns SPEC_COUNT.
TEST_F(SpecTest, NoMatchReturnsSpecCount)
{
    unitTestBidirDshot = true;
    // configs are zeroed in SetUp(), matching no preset.
    EXPECT_EQ(SPEC_COUNT, getCurrentSpec());
}

// 5. Bidir-dshot gate.
TEST_F(SpecTest, BidirDshotGate)
{
    setSpec(SPEC_FREEDOM);
    unitTestBidirDshot = false;
    EXPECT_EQ(SPEC_COUNT, getCurrentSpec());
    EXPECT_FALSE(checkSpec(SPEC_FREEDOM));
}
