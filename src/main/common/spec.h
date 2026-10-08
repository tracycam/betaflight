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

#ifdef USE_KAACK_SPEC

#include <stdbool.h>
#include <stdint.h>

#define MAX_SPEC_NAME_SIZE 16

// Enum representing different types of specs
typedef enum {
    SPEC_FREEDOM,
    SPEC_MGP_PRO,
    SPEC_MAYHEM,
    SPEC_TT,
    SPEC_LLIGUETA,
    SPEC_COUNT // must be last
} SpecType;

// Struct representing the settings for each spec type
typedef struct specSettings_s {
    char name[MAX_SPEC_NAME_SIZE]; // Null-terminated name string
    bool rpm_limit;
    uint16_t rpm_limit_p;
    uint16_t rpm_limit_i;
    uint16_t rpm_limit_d;
    uint16_t rpm_limit_value;
    uint8_t motorPoleCount;
    uint16_t kv;
} specSettings_t;

// Declaration of specArray for external access
extern specSettings_t specArray[];

bool checkSpec(SpecType specType);
void setSpec(SpecType specType);
SpecType getCurrentSpec(void);

#endif // USE_KAACK_SPEC
