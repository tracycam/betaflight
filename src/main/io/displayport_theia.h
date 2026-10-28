/* Internal H415 display, independent of UART MSP displayports. */
#pragma once
#include <stdbool.h>
struct displayPort_s;
struct displayPort_s *displayPortTheiaInit(void);
