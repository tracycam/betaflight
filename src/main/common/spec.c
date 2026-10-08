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
#include <string.h>

#include "platform.h"

#ifdef USE_KAACK_SPEC

#include "common/spec.h"

#include "drivers/motor.h"

#include "flight/mixer.h"

#include "pg/motor.h"

SpecType getCurrentSpec(void)
{
    for (int spec = 0; spec < SPEC_COUNT; spec++) {
        if (checkSpec(spec)) {
            return spec;
        }
    }

    return SPEC_COUNT;
}

bool checkSpec(SpecType specType)
{
    return
        isMotorProtocolBidirDshot() &&
        mixerConfig()->rpm_limit == specArray[specType].rpm_limit &&
        mixerConfig()->rpm_limit_value == specArray[specType].rpm_limit_value &&
        motorConfig()->motorPoleCount == specArray[specType].motorPoleCount &&
        mixerConfig()->rpm_limit_p == specArray[specType].rpm_limit_p &&
        mixerConfig()->rpm_limit_i == specArray[specType].rpm_limit_i &&
        mixerConfig()->rpm_limit_d == specArray[specType].rpm_limit_d;
}

void setSpec(SpecType specType)
{
    mixerConfigMutable()->rpm_limit = specArray[specType].rpm_limit;
    mixerConfigMutable()->rpm_limit_value = specArray[specType].rpm_limit_value;
    motorConfigMutable()->motorPoleCount = specArray[specType].motorPoleCount;
    motorConfigMutable()->kv = specArray[specType].kv;
    mixerConfigMutable()->rpm_limit_p = specArray[specType].rpm_limit_p;
    mixerConfigMutable()->rpm_limit_i = specArray[specType].rpm_limit_i;
    mixerConfigMutable()->rpm_limit_d = specArray[specType].rpm_limit_d;
}

// Definition of specArray, initialized with sample data
specSettings_t specArray[] = {
    {
        "FREEDOM 18K", // Name
        true,    // bool rpm_limit;
        25,      // uint16_t rpm_limit_p;
        10,      // uint16_t rpm_limit_i;
        8,       // uint16_t rpm_limit_d;
        18000,   // uint16_t rpm_limit_value;
        14,      // uint8_t motorPoleCount;
        1960,    // uint16_t kv;
    },

    {
        "MGP PRO 13K", // Name
        true,    // bool rpm_limit;
        25,      // uint16_t rpm_limit_p;
        10,      // uint16_t rpm_limit_i;
        8,       // uint16_t rpm_limit_d;
        13000,   // uint16_t rpm_limit_value;
        14,      // uint8_t motorPoleCount;
        1300,    // uint16_t kv;
    },

    {
        "MAYHEM 24K", // Name
        true,    // bool rpm_limit;
        25,      // uint16_t rpm_limit_p;
        10,      // uint16_t rpm_limit_i;
        8,       // uint16_t rpm_limit_d;
        24000,   // uint16_t rpm_limit_value;
        14,      // uint8_t motorPoleCount;
        1960,    // uint16_t kv;
    },

    {
        "TT 30K", // Name
        true,    // bool rpm_limit;
        25,      // uint16_t rpm_limit_p;
        10,      // uint16_t rpm_limit_i;
        8,       // uint16_t rpm_limit_d;
        30000,   // uint16_t rpm_limit_value;
        12,      // uint8_t motorPoleCount;
        4533,    // uint16_t kv;
    },

    {
        "LA LLIGUETA 17K", // Name
        true,    // bool rpm_limit;
        25,      // uint16_t rpm_limit_p;
        10,      // uint16_t rpm_limit_i;
        8,       // uint16_t rpm_limit_d;
        17000,   // uint16_t rpm_limit_value;
        14,      // uint8_t motorPoleCount;
        1980,    // uint16_t kv;
    },
};

#endif // USE_KAACK_SPEC
