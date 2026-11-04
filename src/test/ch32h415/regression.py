from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
temporary_directory=tempfile.TemporaryDirectory(prefix='ch32h415-regression-')
out=Path(temporary_directory.name)
common='#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\n'
def fn(rel,marker):
 p=root/rel;s=p.read_text();start=s.index(marker);b=s.index('{',start);n=1;i=b+1
 while n:
  if s[i]=='{':n+=1
  if s[i]=='}':n-=1
  i+=1
 return '#line %d "%s"\n'%(s[:start].count('\n')+1,p)+s[start:i]+'\n#line 1 "audit_harness"\n'
def run(name,src,timeout=2,extra_flags=()):
 p=out/(name+'.c');p.write_text(common+src)
 cmd=['gcc','-std=gnu11','-O1','-g','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-Wno-pointer-to-int-cast',*extra_flags,str(p),'-o',str(out/name)]
 c=subprocess.run(cmd,capture_output=True,text=True)
 if c.returncode:
  raise RuntimeError(f'{name}: build failed\n{c.stderr}')
 try:
  r=subprocess.run([str(out/name)],capture_output=True,text=True,timeout=timeout,check=True)
  print(name,'PASS',(r.stdout+r.stderr).strip())
 except subprocess.TimeoutExpired as e:
  raise RuntimeError(f'{name}: timed out\n{(e.stdout or b"").decode()}')
 except subprocess.CalledProcessError as e:
  raise RuntimeError(f'{name}: failed with exit code {e.returncode}\n{e.stdout}{e.stderr}')

pf='src/platform/CH32/include/platform/platform.h'
run('micros_global_irq_disabled', '''
#define CH32H4
typedef struct { uint32_t GISR; } pfic_t;
typedef struct { uint32_t CNT; } systick_t;
static pfic_t pfic;
static systick_t tick;
#define PFIC (&pfic)
#define SysTick1 (&tick)
static volatile uint32_t sysTickUptime, sysTickValStamp;
static uint32_t usTicks=350;
static bool irqEnabled;
static uint32_t __get_BASEPRI(void) { return 0; }
static bool ch32GlobalInterruptsEnabled(void) { return irqEnabled; }
static uint32_t microsISR(void) { return 1234; }
''' + fn('src/platform/common/stm32/system.c','uint32_t micros(void)') + '''
int main(void) {
    tick.CNT=200;
    if (micros() != 1234) return 1;
    irqEnabled=true;
    sysTickValStamp=349999;
    tick.CNT=175000;
    if (micros() != 500) return 2;
    puts("global masking uses the non-spinning path; first tick returns normally");
}
''')
run('ch32_adc_temperature', '''
static uint32_t fakeCalibration;
#define CH32_TS_CAL_ADDRESS ((uintptr_t)&fakeCalibration)
''' + fn('src/platform/CH32/adc_ch32h41x.c','int16_t adcInternalComputeTemperature(') + '''
int main(void) {
    fakeCalibration=(30u << 16) | 1000u;
    if (adcInternalComputeTemperature(1000, 4096) != 30) return 1;
    if (adcInternalComputeTemperature(997, 4096) != 31) return 2;
    if (adcInternalComputeTemperature(1003, 4096) != 29) return 3;
    if (adcInternalComputeTemperature(998, 4096) != 30) return 4;
    if (adcInternalComputeTemperature(1002, 4096) != 30) return 5;
    (void)adcInternalComputeTemperature(UINT16_MAX, UINT16_MAX);
    puts("factory reference and symmetric rounding above and below calibration");
}
''')
run('exti_unregistered', '''
typedef struct extiCallbackRec_s extiCallbackRec_t;
struct extiCallbackRec_s { void (*fn)(extiCallbackRec_t *); };
static struct { extiCallbackRec_t *handler; } extiChannelRecs[16];
static uint32_t imr, pr;
#define EXTI_REG_IMR imr
#define EXTI_REG_PR pr
static int calls;
static void callback(extiCallbackRec_t *self) { (void)self; calls++; }
''' + fn('src/platform/CH32/exti_ch32.c','static void EXTI_IRQHandler(') + '''
int main(void) {
    extiCallbackRec_t valid={callback}, empty={NULL};
    extiChannelRecs[3].handler=&valid;
    extiChannelRecs[2].handler=&empty;
    imr=pr=0x0f;
    EXTI_IRQHandler(0x07);
    if (calls != 0 || pr != 7) return 1;
    pr=0x0f;
    EXTI_IRQHandler(0x0f);
    if (calls != 1 || pr != 15) return 2;
    puts("missing callbacks are skipped; registered callbacks still run");
}
''')
host_include=out/'include'
host_include.mkdir(exist_ok=True)
(host_include/'platform.h').write_text('/* The host tests supply simulated peripherals. */\n')
run('priority_saturation', '''
#include "platform/priority.h"
int main(void) {
    for (unsigned base=0; base<32; base++) {
        const unsigned expected = ((base > 3 ? 3 : base) << 6) | 0x20;
        if (NVIC_BUILD_PRIORITY(base, 0) != expected) return 1;
    }
    puts("out-of-range priority groups saturate at the lowest group");
}
''', extra_flags=('-Wall','-Wextra','-I'+str(root/'src/platform/CH32/include')))
run('dma_flag_masks', '''
typedef struct { uint32_t INTFR, INTFCR; } DMA_TypeDef;
typedef struct { DMA_TypeDef *dma; unsigned flagsShift; } descriptor_t;
#include "platform/dma.h"
int main(void) {
    DMA_TypeDef regs = {0};
    descriptor_t descriptors[2] = {{&regs, 0}, {&regs, 0}};
    for (unsigned channel = 0; channel < 8; channel++) {
        descriptors[1].flagsShift = channel * 4;
        const uint32_t expected = (DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF) << descriptors[1].flagsShift;
        DMA_CLEAR_FLAG(&descriptors[1], DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF);
        if (regs.INTFCR != expected) return 1;
        regs.INTFR = expected;
        if (DMA_GET_FLAG_STATUS(&descriptors[1], DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF) != expected) return 2;
    }
    puts("combined flags target the selected channel");
}
''', extra_flags=('-Wall','-Wextra','-I'+str(host_include),'-I'+str(root/'src/main'),'-I'+str(root/'src/platform/CH32/include')))
run('theia_dma_reservation', '''
#define USE_PLATFORM_DMA_FILTER
#define DMA2_CH6_HANDLER 14
#define DMA_IDENTIFIER_TO_INDEX(x) ((x)-1)
#define OWNER_FREE 0
typedef int dmaIdentifier_e;
typedef int resourceOwner_e;
typedef struct { struct { int owner, index; } resourceOwner; } dmaChannelDescriptor_t;
static dmaChannelDescriptor_t dmaDescriptors[16];
static int dmaGetHandlerCount(void) { return 16; }
''' + fn('src/platform/CH32/dma_ch32h41x.c','bool dmaIsReserved(')
    + fn('src/main/drivers/dma.c','bool dmaAllocate(') + '''
int main(void) {
    if (dmaAllocate(DMA2_CH6_HANDLER, 2, 1)) return 1;
    if (dmaDescriptors[13].resourceOwner.owner != OWNER_FREE) return 2;
    if (!dmaAllocate(13, 2, 1) || dmaAllocate(13, 3, 1)) return 3;
    if (dmaAllocate(0, 2, 1) || dmaAllocate(17, 2, 1)) return 4;
    puts("Theia DMA channel remains reserved; ordinary allocation is unchanged");
}
''')
run('rcc_high_bit', '''
typedef uint32_t rccPeriphTag_t;
typedef enum { DISABLE, ENABLE } FunctionalState;
static struct { uint32_t HBPRSTR, HB1PRSTR, HB2PRSTR; } registers;
#define RCC (&registers)
#define RCC_HB 1
#define RCC_HB1 2
#define RCC_HB2 3
#define NOSUFFIX
''' + fn('src/platform/CH32/rcc_ch32.c','void RCC_ResetCmd(') + '''
int main(void) {
    RCC_ResetCmd((RCC_HB1 << 5) | 31, DISABLE);
    if (registers.HB1PRSTR != 0x80000000u) return 1;
    RCC_ResetCmd((RCC_HB1 << 5) | 31, ENABLE);
    if (registers.HB1PRSTR != 0) return 2;
    puts("bit 31 and reset enable semantics");
}
''')
run('spi_divisor_rounding', '''
#define UNUSED(x) ((void)(x))
typedef struct { int unused; } SPI_TypeDef;
''' + fn('src/platform/CH32/bus_spi_ch32h41x.c','static uint16_t spiDivisorToBRbits(') + '''
int main(void) {
    for (unsigned divisor=0; divisor<=UINT16_MAX; divisor++) {
        const unsigned actual = 2u << (spiDivisorToBRbits(NULL, divisor) >> 3);
        const unsigned minimum = divisor < 2 ? 2 : divisor > 256 ? 256 : divisor;
        if (actual < minimum || actual > 256 || (actual > 2 && actual/2 >= minimum)) return 1;
    }
    puts("all 16-bit divisors select the smallest supported non-faster clock");
}
''')
run('flash_fast_wait', '''
#define ProgramTimeout 8
static struct { uint32_t STATR; } registers;
#define FLASH (&registers)
''' + fn('lib/main/CH32H41x/Peripheral/src/ch32h417_flash.c','static uint32_t FLASH_FastWait(') + '''
int main(void) {
    registers.STATR=0;
    if (!FLASH_FastWait(3)) return 1;
    registers.STATR=1;
    if (FLASH_FastWait(1)) return 2;
    if (!FLASH_FastWait(2)) return 3;
    puts("busy and write-busy polling terminate");
}
''')
run('flash_fast_timeout', '''
#define SR_BSY 1u
#define SR_WR_BSY 2u
#define CR_PAGE_PG 0x10000u
#define CR_PG_STRT 0x200000u
typedef enum { FLASH_COMPLETE, FLASH_TIMEOUT } FLASH_Status;
static struct { uint32_t STATR, CTLR; } registers;
#define FLASH (&registers)
static unsigned waits, failAt, restored;
static uint32_t FLASH_FastIrqSave(void) { return 0x88; }
static void FLASH_FastIrqRestore(uint32_t state) { restored=state; }
static uint32_t FLASH_FastWait(uint32_t mask) {
    waits++;
    if (waits == failAt) { registers.STATR=mask; return 0; }
    return 1;
}
static uint32_t flash[64] __attribute__((aligned(256)));
''' + fn('lib/main/CH32H41x/Peripheral/src/ch32h417_flash.c','FLASH_Status FLASH_ProgramPage_Fast(') + '''
int main(void) {
    uint32_t input[64]; for (unsigned i=0; i<64; i++) input[i]=i+1;
    if ((uintptr_t)flash > UINT32_MAX) return 1;
    failAt=1; registers.STATR=0; registers.CTLR=0; waits=restored=0;
    if (FLASH_ProgramPage_Fast((uint32_t)(uintptr_t)flash,input) != FLASH_TIMEOUT || registers.CTLR || restored!=0x88) return 2;
    failAt=2; registers.STATR=0; registers.CTLR=0; waits=restored=0; memset(flash,0,sizeof(flash));
    if (FLASH_ProgramPage_Fast((uint32_t)(uintptr_t)flash,input) != FLASH_TIMEOUT || flash[0]!=1 || flash[1] || !(registers.CTLR & CR_PAGE_PG) || (registers.CTLR & CR_PG_STRT) || restored!=0x88) return 3;
    failAt=0; registers.STATR=0; registers.CTLR=0; waits=restored=0;
    if (FLASH_ProgramPage_Fast((uint32_t)(uintptr_t)flash,input) != FLASH_COMPLETE || memcmp(flash,input,sizeof(flash)) || (registers.CTLR & CR_PAGE_PG) || restored!=0x88) return 4;
    puts("timeout stops writes, preserves busy mode and restores interrupts");
}
''',extra_flags=('-fno-pie','-no-pie'))
run('ch32_config_flash_bounds', '''
#define CH32H4
#define CONFIG_STREAMER_BUFFER_SIZE 256
#define STATIC_ASSERT(c, m) _Static_assert(c, m)
typedef uint32_t config_streamer_buffer_type_t;
typedef enum { CONFIG_RESULT_SUCCESS=0, CONFIG_RESULT_FAILURE=-1, CONFIG_RESULT_ADDRESS_INVALID=-3, CONFIG_RESULT_TIMEOUT=-4 } configStreamerResult_e;
typedef enum { FLASH_COMPLETE, FLASH_ERROR, FLASH_TIMEOUT } FLASH_Status;
static uint8_t flash[16384] __attribute__((aligned(8192)));
#define __config_start flash[0]
#define __config_end flash[16384]
static uint32_t flashMode;
#define FLASH_CFGR0_BASE ((uintptr_t)&flashMode)
static unsigned erases, writes, eraseStep;
static bool suppressWrite;
static bool timeoutWrite;
static FLASH_Status FLASH_ErasePage(uintptr_t addr) {
    if (addr != (uintptr_t)flash + erases * eraseStep) return FLASH_ERROR;
    erases++;
    return FLASH_COMPLETE;
}
static FLASH_Status FLASH_ProgramPage_Fast(uintptr_t addr, uint32_t *buf) {
    if (timeoutWrite) return FLASH_TIMEOUT;
    if (!suppressWrite) memcpy((void *)addr, buf, CONFIG_STREAMER_BUFFER_SIZE);
    writes++;
    return FLASH_COMPLETE;
}
static FLASH_Status FLASH_GetStatus(void) { return FLASH_COMPLETE; }
''' + fn('src/platform/common/stm32/config_flash.c','static bool ch32ConfigAddressIsValid(')
    + fn('src/platform/common/stm32/config_flash.c','configStreamerResult_e configWriteWord(') + '''
int main(void) {
    uint32_t buffer[64]={0};
    for (int dual=0; dual<2; dual++) {
        erases=writes=0;
        flashMode=dual ? 1u<<28 : 0;
        eraseStep=dual ? 8192 : 4096;
        if (configWriteWord((uintptr_t)flash-256, buffer) != CONFIG_RESULT_ADDRESS_INVALID) return 1;
        if (configWriteWord((uintptr_t)flash+sizeof(flash), buffer) != CONFIG_RESULT_ADDRESS_INVALID) return 2;
        if (configWriteWord((uintptr_t)flash+1, buffer) != CONFIG_RESULT_ADDRESS_INVALID) return 3;
        if (configWriteWord(UINTPTR_MAX-127, buffer) != CONFIG_RESULT_ADDRESS_INVALID) return 4;
        for (unsigned offset=0; offset<sizeof(flash); offset+=256)
            if (configWriteWord((uintptr_t)flash+offset, buffer) != CONFIG_RESULT_SUCCESS) return 5;
        if (erases != sizeof(flash)/eraseStep || writes != sizeof(flash)/256) return 6;
    }
    memset(flash, 0xff, 256);
    suppressWrite=true;
    erases=0;
    if (configWriteWord((uintptr_t)flash, buffer) != CONFIG_RESULT_FAILURE) return 7;
    erases=0;
    timeoutWrite=true;
    if (configWriteWord((uintptr_t)flash, buffer) != CONFIG_RESULT_TIMEOUT) return 8;
    puts("single/dual erase geometry, boundaries, timeout and silent write failure");
}
''')
run('pfic', '#define PLATFORM_CUSTOM_ATOMIC_BLOCK_ALL\n#include "platform/priority.h"\nstatic volatile uint32_t threshold;\n#define PFIC_ITHRESDR threshold\n'
    +fn(pf,'__attribute__((always_inline)) static inline uint32_t __get_BASEPRI')
    +fn(pf,'__attribute__((always_inline)) static inline void __set_BASEPRI_nb')
    +fn(pf,'__attribute__((always_inline)) static inline void __set_BASEPRI_MAX_nb')
    +(root/'src/test/ch32h415/pfic_test.c').read_text(),
    extra_flags=('-Wall','-Wextra','-I'+str(host_include),'-I'+str(root/'src/main'),'-I'+str(root/'src/platform/CH32/include')))

platform_source=(root/pf).read_text()
macro_start=platform_source.index('#define ATOMIC_BLOCK_ALL()')
macro_end=platform_source.index('\n', platform_source.index('\n', macro_start)+1)
atomic_macro=platform_source[macro_start:macro_end]
run('global_irq_scope', '''
static uint32_t csr;
static uint32_t ch32IrqSave(void) { uint32_t state = csr & 0x88u; csr &= ~0x88u; return state; }
static void ch32IrqRestore(uint32_t *state) { csr |= *state; }
''' + atomic_macro + '''
static int earlyReturn(void) { ATOMIC_BLOCK_ALL() { return csr == 0; } return 0; }
int main(void) {
    for (unsigned i = 0; i < 4; i++) {
        csr = ((i & 1) ? 0x08 : 0) | ((i & 2) ? 0x80 : 0);
        const uint32_t initial = csr;
        ATOMIC_BLOCK_ALL() {
            if (csr != 0) return 1;
            ATOMIC_BLOCK_ALL() { if (csr != 0) return 2; }
            if (csr != 0) return 3;
        }
        if (csr != initial || !earlyReturn() || csr != initial) return 4;
        ATOMIC_BLOCK_ALL() { break; }
        if (csr != initial) return 5;
    }
    puts("nested, return and break preserve each enable bit");
}
''')

bb='src/platform/CH32/dshot_bitbang.c'
run('dshot_dma_recovery', '''
#define USE_DSHOT_TELEMETRY
#define DISABLE 0
#define ENABLE 1
#define DSHOT_BITBANG_DIRECTION_INPUT 1
#define DSHOT_BITBANG_DIRECTION_OUTPUT 0
#define DEBUG_DSHOT_TELEMETRY_COUNTS 0
#define DEBUG_SET(a,b,c) (debug[b] = (c))
typedef int32_t timeDelta_t;
typedef uint32_t timeUs_t;
typedef struct { void *tim; uint16_t dmaSources, armedOutputSources, pendingOutputSources, pendingInputSources, outputARR, inputARR; } bbPacer_t;
typedef struct { bbPacer_t *pacer; uint16_t dmaSource, outputARR, inputARR; bool telemetryPending, telemetryAborted, reinitRequired, inputActive; int direction; } bbPort_t;
static bbPort_t bbPorts[2];
static bbPacer_t bbPacers[1];
static int usedMotorPorts=2, usedMotorPacers, dshotMotorCount, debug[4];
static bool useDshotTelemetry=true, stopSucceeds;
static int switches, enables;
static timeUs_t now, lastSendUs;
static timeDelta_t bbTelemetryTimeoutUs=10;
static timeUs_t micros(void) { return now++; }
static timeDelta_t cmpTimeUs(timeUs_t a, timeUs_t b) { return (timeDelta_t)(a-b); }
static bool dshotCommandQueueEmpty(void) { return true; }
static bool dshotCommandOutputIsEnabled(int n) { (void)n; return true; }
static bool bbSwitchToOutput(bbPort_t *p) {
    switches++;
    if (!stopSucceeds) return false;
    p->direction = DSHOT_BITBANG_DIRECTION_OUTPUT;
    return true;
}
static void bbDMA_Cmd(bbPort_t *p, int enabled) { (void)p; enables += enabled; }
static void bbTIM_ReloadPeriod(void *tim, uint16_t period) { (void)tim; (void)period; }
static void bbTIM_DMACmd(void *tim, uint16_t sources, int enabled) { (void)tim; (void)sources; (void)enabled; }
''' + fn(bb,'static bool bbTelemetryWait(void)') + fn(bb,'static void bbUpdateComplete(void)') + '''
int main(void) {
    bbPorts[0] = (bbPort_t){ .telemetryPending=true, .direction=DSHOT_BITBANG_DIRECTION_INPUT, .reinitRequired=true };
    if (!bbTelemetryWait() || !bbPorts[0].telemetryAborted || bbPorts[1].telemetryAborted || debug[2] != 1) return 1;
    bbUpdateComplete();
    if (!bbPorts[0].reinitRequired || !bbPorts[0].telemetryPending || enables != 1) return 2;
    stopSucceeds=true;
    switches=enables=0;
    bbUpdateComplete();
    if (bbPorts[0].reinitRequired || bbPorts[0].telemetryPending || switches != 1 || enables != 2) return 3;
    puts("timeout isolates partial capture; stopped DMA is required before recovery");
}
''')
run('dshot_dma_stop', '''
#define DISABLE 0
#define ENABLE 1
#define DSHOT_BITBANG_DIRECTION_OUTPUT 0
#define DMA_IT_TC 1
#define DMA_IT_TE 2
#define NVIC_PRIO_TIMER 0
#define ATOMIC_BLOCK(x) for (int once=1; once; once=0)
#define WRITE_REG(reg, value) ((reg)=(value))
#define MODIFY_REG(reg, mask, value) ((reg)=((reg)&~(mask))|(value))
#define dbgPinHi(x) ((void)0)
#define dbgPinLo(x) ((void)0)
typedef struct { uint32_t CFGR; } dmaResource_t;
typedef struct { uint32_t BSHR, CFGLR, CFGHR; } gpio_t;
typedef struct { uint32_t ATRLR; } TIM_TypeDef;
typedef struct { TIM_TypeDef *tim; } mockTimer_t;
typedef struct { dmaResource_t *dmaResource; gpio_t *gpio; mockTimer_t *timhw; uint32_t gpioIdleBSRR, gpioModeMask, gpioModeOutput, outputARR, gpioCfgLrMask, gpioCfgHrMask, gpioCfgLrOutput, gpioCfgHrOutput; int outputDmaInit, direction; } bbPort_t;
static bool stuck;
static int dmaWrites;
static void bbDMA_Cmd(bbPort_t *p, int state) { if (!stuck) p->dmaResource->CFGR=state; }
#define IS_DMA_ENABLED(d) ((d)->CFGR & 1u)
#define BB_DMA_STOP_SPIN_LIMIT 100
static void xDMA_Init(dmaResource_t *d, int *init) { (void)d; (void)init; dmaWrites++; }
static void xDMA_ITConfig(dmaResource_t *d, int flags, int state) { (void)d; (void)flags; (void)state; }
''' + fn('src/platform/common/stm32/dshot_bitbang_impl.h','static inline bool bbDMAWaitStopped(')
    + fn('src/platform/CH32/dshot_bitbang_stdperiph.c','bool bbSwitchToOutput(') + '''
int main(void) {
    dmaResource_t dma={.CFGR=1}; gpio_t gpio={.BSHR=17}; TIM_TypeDef tim={.ATRLR=42}; mockTimer_t hw={&tim};
    bbPort_t port={.dmaResource=&dma,.gpio=&gpio,.timhw=&hw,.gpioIdleBSRR=7,.outputARR=99,.direction=1};
    stuck=true;
    if (bbSwitchToOutput(&port) || gpio.BSHR != 17 || tim.ATRLR != 42 || dmaWrites || port.direction != 1) return 1;
    stuck=false;
    if (!bbSwitchToOutput(&port) || gpio.BSHR != 7 || tim.ATRLR != 99 || dmaWrites != 1 || port.direction != 0) return 2;
    puts("busy channel leaves GPIO, timer and DMA configuration untouched");
}
''')
pre='''
#define USE_DMA_SPEC
#define MAX_PERIPHERAL_DMA_OPTIONS 16
#define MAX_SUPPORTED_MOTOR_PORTS 4
#define ARRAYLEN(x) (sizeof(x)/sizeof((x)[0]))
#define CC_CHANNELS_PER_TIMER 4
#define CC_CHANNEL_FROM_INDEX(x) ((x)*4)
#define DSHOT_BITBANGED_TIMER_TIM1 1
#define DSHOT_BITBANGED_TIMER_TIM8 8
#define OWNER_FREE 0
#define OWNER_DSHOT_BITBANG 1
#define TIM1 1
#define TIM8 8
#define NONE 0
typedef int resourceOwner_e;
typedef int dmaoptValue_t;
typedef int dmaIdentifier_e;
typedef int dmaResource_t;
typedef struct {int tim, channel, opt;} timerHardware_t;
#define DEF_TIM(t,ch,tag,usage,dma,output) {t,ch,dma}
#define CH1 0
#define CH2 4
#define CH3 8
#define CH4 12
static const timerHardware_t bbTimerHardware[] = {
 DEF_TIM(TIM8,CH1,NONE,0,8,0),DEF_TIM(TIM1,CH1,NONE,0,9,0),DEF_TIM(TIM8,CH2,NONE,0,10,0),DEF_TIM(TIM1,CH2,NONE,0,11,0),
 DEF_TIM(TIM8,CH3,NONE,0,12,0),DEF_TIM(TIM1,CH3,NONE,0,14,0),DEF_TIM(TIM8,CH4,NONE,0,15,0),DEF_TIM(TIM1,CH4,NONE,0,0,0)};
struct { const timerHardware_t *timhw; dmaResource_t *dmaResource; uint32_t dmaChannel; } bbPorts[4];
typedef struct {int owner;} resourceOwner_t;
static resourceOwner_t freeOwner={0},busyOwner={2};
static struct {struct {int useDshotBitbangedTimer;}dev;} config;
#define motorConfig() (&config)
static int lookupTimerIndex(int tim){return tim;}
static int timerGetNumberByIndex(int idx){return idx;}
static const timerHardware_t *timerGetAllocatedByNumberAndChannel(int t,int c){return NULL;}
static const resourceOwner_t *timerGetOwner(const timerHardware_t *t){return &freeOwner;}
static dmaoptValue_t dmaGetOptionByTimer(const timerHardware_t *t){return t->opt;}
typedef struct {dmaResource_t *ref; uint32_t dmaMuxId;} dmaChannelSpec_t;
static int channel;
static const dmaChannelSpec_t *dmaGetChannelSpecByTimerValue(int t,int c,int o){static const dmaChannelSpec_t s={&channel,1}; return &s;}
static dmaIdentifier_e dmaGetIdentifier(const dmaResource_t *d){return 1;}
static const resourceOwner_t *dmaGetOwner(int id){return &busyOwner;}
'''
run('pacer_exhaustion',pre+fn('src/platform/CH32/timer_ch32bsp.c','int8_t timerGetTIMNumber(')+fn(bb,'static void bbFindPacerTimer(void)')+'int main(void){bbFindPacerTimer();puts("returned without dereferencing null");}\n')

pre='''
#define MAX_SUPPORTED_MOTORS 4
#define TIMER_OUTPUT_INVERTED 1
#define BB_GPIO_PULLDOWN 1
#define BB_GPIO_PULLUP 2
#define DSHOT_BITBANG_STATUS_OK 0
#define DSHOT_BITBANG_STATUS_MOTOR_PIN_CONFLICT 1
#define DIR_OUT 3
#define GPIO_MODE_OUT_AF_PP 2
#define GPIO_SPEED_VERY_HIGH 3
#define IO_CONFIG(a,b,c,d) 0
#define OWNER_MOTOR 1
#define RESOURCE_INDEX(x) ((x)+1)
#define dbgPinLo(x) ((void)0)
typedef int motorProtocolTypes_e;
typedef void *IO_t;
typedef struct {int output;} timerHardware_t;
typedef struct {unsigned motorOutputReordering[4];unsigned ioTags[4]; int motorInversion,motorProtocol;} motorDevConfig_t;
typedef struct {unsigned count;const int *vTable;} motorDevice_t;
static int bbVTable,motorProtocol,dshotMotorCount,bbStatus,bbPuPdMode,bbOutputBuffer[1];
static struct {int pinIndex,output,iocfg;IO_t io;} bbMotors[4];
static const timerHardware_t *timerGetConfiguredByTag(unsigned tag){return NULL;}
static IO_t IOGetByTag(unsigned tag){return NULL;}
static bool IOIsFreeOrPreinit(IO_t io){return true;}
static int IO_GPIOPinIdx(IO_t io){return -1;}
static void IOInit(IO_t a,int b,int c){}
static void IOConfigGPIO(IO_t a,int b){}
static void IOLo(IO_t a){}
static void IOHi(IO_t a){}
'''
run('motor_unconfigured',pre+fn(bb,'bool dshotBitbangDevInit(')+'''int main(void){motorDevice_t d={.count=4}; motorDevConfig_t c={0}; return dshotBitbangDevInit(&d,&c);}''')

run('dshot_bb_gpio_precompute', '''
#define USE_DSHOT_TELEMETRY
#define DIR_IN 0u
#define DIR_OUT 1u
#define GPIO_PULL_NONE 0u
#define GPIO_PULL_DOWN 1u
#define GPIO_PULL_UP 2u
#define IO_CONFIG_GET_PULL(cfg) (((cfg) >> 6) & 3u)
#define DSHOT_BITBANG_DIRECTION_OUTPUT 1
#define DSHOT_BITBANG_DIRECTION_INPUT 2
#define NVIC_PRIO_TIMER 0
#define ATOMIC_BLOCK(x) for (int once=1; once; once=0)
#define WRITE_REG(r,v) ((r)=(v))
#define MODIFY_REG(r,m,v) ((r)=((r)&~(m))|(v))
#define dbgPinHi(x) ((void)0)
#define dbgPinLo(x) ((void)0)
#define DISABLE 0
#define ENABLE 1
#define DMA_IT_TC 1
#define DMA_IT_TE 2
typedef struct { uint32_t CFGLR, CFGHR, BSHR; } GPIO_TypeDef;
typedef struct { uint32_t ATRLR, CNT; } TIM_TypeDef;
typedef struct { int unused; } dmaResource_t;
typedef struct { void *tim; } timerHardware_t;
typedef struct { GPIO_TypeDef *gpio; dmaResource_t *dmaResource; timerHardware_t *timhw;
    uint32_t gpioModeMask,gpioModeInput,gpioModeOutput,gpioIdleBSRR;
    uint32_t gpioCfgLrMask,gpioCfgHrMask,gpioCfgLrOutput,gpioCfgHrOutput,gpioCfgLrInput,gpioCfgHrInput,gpioInputBSRR;
    uint32_t outputDmaInit,inputDmaInit,outputARR,inputARR,direction;
} bbPort_t;
typedef struct { bbPort_t *bbPort; int pinIndex; unsigned iocfg; void *io; } bbMotor_t;
static bool useDshotTelemetry=true;
static void IOWrite(void *io,int value) { (void)io;(void)value; }
static void bbDMA_Cmd(bbPort_t *p,int state) { (void)p;(void)state; }
static bool bbDMAWaitStopped(dmaResource_t *p) { (void)p;return true; }
static void xDMA_Init(dmaResource_t *p,void *init) { (void)p;(void)init; }
static void xDMA_ITConfig(dmaResource_t *p,int flags,int state) { (void)p;(void)flags;(void)state; }
''' + fn('src/platform/CH32/dshot_bitbang_stdperiph.c','void bbGpioSetup(')
    + fn('src/platform/CH32/dshot_bitbang_stdperiph.c','bool bbSwitchToOutput(')
    + fn('src/platform/CH32/dshot_bitbang_stdperiph.c','void bbSwitchToInput(') + '''
int main(void) {
    GPIO_TypeDef gpio={.CFGLR=0x55555555,.CFGHR=0x55555555};
    TIM_TypeDef timer={0};timerHardware_t hw={&timer};dmaResource_t dma={0};
    bbPort_t p={.gpio=&gpio,.timhw=&hw,.dmaResource=&dma};
    const int pins[]={3,4,5,6,8,15};
    const unsigned pulls[]={GPIO_PULL_UP,GPIO_PULL_DOWN,GPIO_PULL_UP,GPIO_PULL_DOWN,GPIO_PULL_NONE,GPIO_PULL_DOWN};
    for (unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++) {
        bbMotor_t m={.bbPort=&p,.pinIndex=pins[i],.iocfg=pulls[i]<<6};bbGpioSetup(&m);
    }
    if (p.gpioCfgLrMask!=0x0ffff000u || p.gpioCfgHrMask!=0xf000000fu) return 1;
    if (p.gpioCfgLrOutput!=0x03333000u || p.gpioCfgHrOutput!=0x30000003u) return 2;
    if (p.gpioCfgLrInput!=0x08888000u || p.gpioCfgHrInput!=0x80000004u) return 3;
    if (p.gpioInputBSRR!=((1u<<3)|(1u<<5)|(1u<<20)|(1u<<22)|(1u<<31))) return 4;
    if (!bbSwitchToOutput(&p) || gpio.CFGLR!=0x53333555u || gpio.CFGHR!=0x35555553u || gpio.BSHR!=p.gpioIdleBSRR) return 5;
    bbSwitchToInput(&p);
    if (gpio.CFGLR!=0x58888555u || gpio.CFGHR!=0x85555554u || gpio.BSHR!=p.gpioInputBSRR) return 6;
    puts("mixed low/high pins, pull directions, and untouched GPIO bits are preserved");
}
''')
run('dshot_bb_port_reuse', '''
#define MAX_SUPPORTED_MOTOR_PORTS 1
#define DSHOT_BITBANG_STATUS_TOO_MANY_PORTS 1
#define DSHOT_BITBANG_STATUS_NO_PACER 2
#define OWNER_DSHOT_BITBANG 3
#define RESOURCE_INDEX(i) ((i)+1)
typedef struct { const void *timhw; int portIndex; struct { int owner,index; } resourceOwner;
    uint32_t gpioModeMask,gpioModeInput,gpioModeOutput,gpioIdleBSRR,gpioInputBSRR;
    uint32_t gpioCfgLrMask,gpioCfgHrMask,gpioCfgLrOutput,gpioCfgHrOutput,gpioCfgLrInput,gpioCfgHrInput;
} bbPort_t;
static bbPort_t bbPorts[1];static int usedMotorPorts,bbStatus;
''' + fn('src/platform/CH32/dshot_bitbang.c','static bbPort_t *bbAllocateMotorPort(') + '''
int main(void) {
    int timer;bbPort_t *p=&bbPorts[0];p->timhw=&timer;
    p->gpioModeMask=p->gpioModeInput=p->gpioModeOutput=p->gpioIdleBSRR=p->gpioInputBSRR=~0u;
    p->gpioCfgLrMask=p->gpioCfgHrMask=p->gpioCfgLrOutput=p->gpioCfgHrOutput=p->gpioCfgLrInput=p->gpioCfgHrInput=~0u;
    if (bbAllocateMotorPort(2)!=p || p->timhw!=&timer || p->portIndex!=2) return 1;
    if (p->gpioModeMask|p->gpioModeInput|p->gpioModeOutput|p->gpioIdleBSRR|p->gpioInputBSRR|
        p->gpioCfgLrMask|p->gpioCfgHrMask|p->gpioCfgLrOutput|p->gpioCfgHrOutput|p->gpioCfgLrInput|p->gpioCfgHrInput) return 2;
    puts("reused port clears cached masks without losing its allocated timer");
}
''')

uart='src/platform/common/stm32/serial_uart_hw.c'
pre='''
#define USE_DMA_SPEC
#define DMA_TRAIT_MUX 1
#define DMA_OPT_UNUSED -1
#define UARTDEV_INVALID -1
#define UART_REG_TXD(base) ((base)->data)
#define UART_REG_RXD(base) ((base)->data)
typedef int serialPortIdentifier_e;
typedef int uartDeviceIdx_e;
typedef int resourceOwner_e;
typedef int dmaIdentifier_e;
typedef struct {uint32_t data;} usart_t;
typedef struct {int identifier,txPriority;usart_t *reg;} uartHardware_t;
typedef struct {void *txDMAResource,*rxDMAResource;int txDMAMuxId,rxDMAMuxId;uint32_t txDMAPeripheralBaseAddr,rxDMAPeripheralBaseAddr;} uartPort_t;
typedef struct {uartPort_t port; const uartHardware_t *hardware;} uartDevice_t;
typedef struct {int txDmaopt,rxDmaopt;} serialUartConfig_t;
typedef struct {void *ref;int dmaMuxId;} dmaChannelSpec_t;
#define DMA_PERIPH_UART_TX 1
#define DMA_PERIPH_UART_RX 2
static int used_channel;
static const dmaChannelSpec_t spec={&used_channel,42};
static const serialUartConfig_t cfg={8,8};
static int uartDeviceIdxFromIdentifier(int x){return 0;}
static int serialResourceIndex(int x){return 0;}
static int serialOwnerIndex(int x){return 1;}
static int serialOwnerTxRx(int x){return 2;}
static const serialUartConfig_t *serialUartConfig(int x){return &cfg;}
static const dmaChannelSpec_t *dmaGetChannelSpecByPeripheral(int a,int b,int c){return &spec;}
static dmaIdentifier_e dmaGetIdentifier(void *r){return 9;}
static bool dmaAllocate(int a,int b,int c){return false;}
static void dmaEnable(int a){}
static void dmaMuxEnable(int a,int b){}
static void uartDmaIrqHandler(void){}
static void dmaSetHandler(int a,void(*f)(void),int c,uint32_t d){}
'''
run('uart_dma_refusal',pre+fn(uart,'void uartConfigureDma(')+'''
int main(void){usart_t reg={0}; uartHardware_t h={1,0,&reg};uartDevice_t d={.hardware=&h};uartConfigureDma(&d);
 printf("allocation denied: txDMAResource=%p rxDMAResource=%p peripheral addresses=%x/%x\\n",d.port.txDMAResource,d.port.rxDMAResource,d.port.txDMAPeripheralBaseAddr,d.port.rxDMAPeripheralBaseAddr);
 return !!d.port.txDMAResource||!!d.port.rxDMAResource;}
''')

usb='src/platform/CH32/serial_usb_vcp_ch32h4.c'
run('usb_cdc_callbacks', '''
#define NVIC_PRIO_USB 0
#define ATOMIC_BLOCK(x) for (int once=1; once; once=0)
#define USB_LOG_DBG(...) ((void)0)
#define USB_LOG_WRN(...) ((void)0)
#define LO_BYTE(x) ((x)&0xff)
#define CDC_REQUEST_SET_LINE_CODING 1
#define CDC_REQUEST_SET_CONTROL_LINE_STATE 2
#define CDC_REQUEST_GET_LINE_CODING 3
#define CDC_REQUEST_SEND_BREAK 4
static const char *stop_name[] = { "1", "1.5", "2" };
static const char *parity_name[] = { "N", "O", "E", "M", "S" };
struct usb_setup_packet { uint8_t bRequest; uint16_t wValue, wIndex, wLength; };
struct cdc_line_coding { uint32_t dwDTERate; uint8_t bCharFormat, bParityType, bDataBits; } __attribute__((packed));
static struct cdc_line_coding cdc_vcp_line_coding;
static void (*baudRateCb)(void *, uint32_t);
static void *baudRateCbContext;
static void (*ctrlLineStateCb)(void *, uint16_t);
static void *ctrlLineStateCbContext;
static bool usbHostPortOpen;
#define CTRL_LINE_STATE_DTR 1
static void usbd_cdc_acm_set_dtr(uint8_t bus, uint8_t intf, bool value) { (void)bus; (void)intf; (void)value; }
static void usbd_cdc_acm_set_rts(uint8_t bus, uint8_t intf, bool value) { (void)bus; (void)intf; (void)value; }
static void usbd_cdc_acm_send_break(uint8_t bus, uint8_t intf) { (void)bus; (void)intf; }
''' + fn(usb,'void CDC_SetBaudRateCb(') + fn(usb,'void CDC_SetCtrlLineStateCb(')
    + fn(usb,'void usbVcpBaudRateChanged(') + fn(usb,'void usbVcpControlLineStateChanged(')
    + fn('lib/main/CH32H41x/middlewares/board/cdc_vcp_ch32h41x.c','void usbd_cdc_acm_set_line_coding(')
    + fn('lib/main/CH32H41x/middlewares/board/cdc_vcp_ch32h41x.c','void usbd_cdc_acm_get_line_coding(')
    + fn('lib/main/CH32H41x/middlewares/board/cdc_vcp_ch32h41x.c','void usbd_cdc_acm_set_control_line_state(')
    + fn('lib/main/CH32H41x/middlewares/class/cdc/usbd_cdc_acm.c','static int cdc_acm_class_interface_request_handler(') + '''
static unsigned baudCalls, stateCalls, seenBaud, seenState;
static void onBaud(void *ctx, uint32_t baud) { if (ctx) { baudCalls++; seenBaud=baud; } }
static void onState(void *ctx, uint16_t state) { if (ctx) { stateCalls++; seenState=state; } }
int main(void) {
    int token=1;
    CDC_SetBaudRateCb(onBaud, &token); CDC_SetCtrlLineStateCb(onState, &token);
    struct cdc_line_coding coding={230400,0,0,8}; uint8_t *data=(uint8_t *)&coding; uint32_t len=7;
    struct usb_setup_packet setup={.bRequest=CDC_REQUEST_SET_LINE_CODING,.wLength=7};
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) || baudCalls!=1 || seenBaud!=230400 || cdc_vcp_line_coding.dwDTERate!=230400) return 1;
    for (unsigned n=0; n<32; n++) {
        if (n==7) continue;
        setup.wLength=n; len=n;
        if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) != -1 || baudCalls!=1) return 4;
    }
    setup.wLength=len=7;
    coding.dwDTERate=0;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) != -1) return 5;
    coding.dwDTERate=0x40000000u;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) != -1) return 6;
    coding.dwDTERate=230400; coding.bParityType=5;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) != -1) return 7;
    coding.bParityType=0; coding.bCharFormat=3;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) != -1 || baudCalls!=1) return 8;
    setup.bRequest=CDC_REQUEST_SET_CONTROL_LINE_STATE; setup.wValue=3;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) || stateCalls!=1 || seenState!=3 || !usbHostPortOpen) return 2;
    CDC_SetCtrlLineStateCb(NULL,NULL); setup.wValue=0;
    if (cdc_acm_class_interface_request_handler(0,&setup,&data,&len) || stateCalls!=1 || usbHostPortOpen) return 3;
    puts("CDC control requests deliver baud and combined line state once");
}
''')
run('usb_tx_empty_state', '''
#define UNUSED(x) ((void)(x))
#define NVIC_PRIO_USB 0
#define ATOMIC_BLOCK(x) for (int once=1; once; once=0)
typedef int serialPort_t;
static struct { uint32_t txAt; } vcpPort;
static uint32_t UserTxBufPtrIn, UserTxBufPtrOut, lastBuffsize;
static uint8_t ep_tx_busy_flag;
static bool usbHostPortOpen, configured;
static bool usbIsConfigured(void) { return configured; }
''' + fn(usb,'static bool isUsbVcpTransmitBufferEmpty(') + '''
int main(void) {
    UserTxBufPtrIn=5; ep_tx_busy_flag=1;
    if (!isUsbVcpTransmitBufferEmpty(NULL)) return 1;
    configured=true;
    if (!isUsbVcpTransmitBufferEmpty(NULL)) return 2;
    usbHostPortOpen=true;
    if (isUsbVcpTransmitBufferEmpty(NULL)) return 3;
    UserTxBufPtrOut=5;
    if (isUsbVcpTransmitBufferEmpty(NULL)) return 4;
    ep_tx_busy_flag=0; lastBuffsize=5;
    if (isUsbVcpTransmitBufferEmpty(NULL)) return 5;
    lastBuffsize=0; vcpPort.txAt=1;
    if (isUsbVcpTransmitBufferEmpty(NULL)) return 6;
    vcpPort.txAt=0;
    if (!isUsbVcpTransmitBufferEmpty(NULL)) return 7;
    puts("disconnected, closed, buffered, queued, in-flight, ZLP and empty states");
}
''')
run('usb_tx_wait_stall', '''
#define PLATFORM_VCP_TX_STALL_TIMEOUT_MS 500
#define USE_VCP
#define SERIAL_PORT_USB_VCP 1
typedef struct { int identifier; } serialPort_t;
static uint32_t now, freeBytes;
static bool empty;
static uint32_t millis(void) { return now; }
static uint32_t serialTxBytesFree(serialPort_t *p) { (void)p; return freeBytes; }
static bool isSerialTransmitBufferEmpty(serialPort_t *p) { (void)p; return empty; }
static void delay(int ms) { now+=ms; if (now == 300) freeBytes++; if (now == 900) empty=true; }
''' + fn('src/main/io/serial.c','void waitForSerialPortToFinishTransmitting(') + '''
int main(void) {
    serialPort_t p={SERIAL_PORT_USB_VCP};
    waitForSerialPortToFinishTransmitting(&p);
    if (now != 800 || empty) return 1;
    now=0; empty=true;
    waitForSerialPortToFinishTransmitting(&p);
    if (now != 0) return 2;
    puts("host stall is bounded and the timeout resets when transmission progresses");
}
''')
for mps in (64, 512):
 run('usb_zlp_mps_'+str(mps), '''
#define APP_TX_DATA_SIZE 2048
#define APP_TX_BLOCK_SIZE 512
#define CDC_MAX_MPS TEST_MPS
static uint8_t UserTxBuffer[APP_TX_DATA_SIZE];
static uint32_t UserTxBufPtrIn, UserTxBufPtrOut, lastBuffsize;
static uint8_t ep_tx_busy_flag;
static unsigned sendCount, lastLength;
static int usb_vcp_send_data(int bus, uint8_t *buffer, unsigned length) {
    (void)bus; (void)buffer;
    sendCount++; lastLength=length; ep_tx_busy_flag=1;
    return 0;
}
''' + fn(usb,'void usb_int_rxsof_handler(void)') + '''
static void tick(void) { for (int i=0; i<17; i++) usb_int_rxsof_handler(); }
int main(void) {
    UserTxBufPtrIn=64;
    tick();
    if (sendCount != 1 || lastLength != 64) return 1;
    ep_tx_busy_flag=0;
    tick();
    if (sendCount != (TEST_MPS == 64 ? 2u : 1u)) return 2;
    ep_tx_busy_flag=0;
    UserTxBufPtrIn=576;
    tick();
    if (lastLength != 512) return 3;
    ep_tx_busy_flag=0;
    unsigned before=sendCount;
    tick();
    if (sendCount != before+1 || lastLength != 0) return 4;
    puts("ZLP follows the configured bulk endpoint packet size");
}
''',extra_flags=('-DTEST_MPS='+str(mps),))
pre='''
#define USB_TIMEOUT 50
#define UNUSED(x) ((void)(x))
#define NVIC_BUILD_PRIORITY(a,b) 0
#define NVIC_PRIO_USB 0
#define ATOMIC_BLOCK(x) for(int once=1;once;once=0)
#define APP_TX_DATA_SIZE 2048
static volatile uint32_t now_ms,UserTxBufPtrIn,UserTxBufPtrOut;
static volatile uint8_t UserTxBuffer[2048];
typedef uint32_t timeUs_t;
typedef int serialPort_t;
static timeUs_t micros(void){return now_ms*1000;}
static uint32_t millis(void){return now_ms;}
static void delay(int ms){now_ms+=ms;}
static bool usbVcpIsConnected(void){return true;}
static bool usbIsConnected(void){return true;}
static bool usbIsConfigured(void){return true;}
'''
run('usb_host_stops_reading',pre+fn(usb,'uint32_t CDC_Send_FreeBytes(void)')+fn(usb,'uint32_t CDC_Send_DATA(')+fn(usb,'static void usbVcpWriteBuf(')+'''
int main(void){uint8_t data[2048]={0};usbVcpWriteBuf(NULL,data,2048);printf("returned after %u ms\\n",now_ms);}
''',timeout=0.3)

uc='src/platform/CH32/serial_uart_ch32bsp.c'
pre='''
#define NVIC_PRIO_SERIALUART_TXDMA 0x50
#define ATOMIC_BLOCK(x) for(int once=1;once;once=0)
#define ENABLE 1
#define DISABLE 0
#define USART_FLAG_TC 1
#define DMA_IT_TCIF 2
#define DMA_IT_HTIF 4
#define DMA_IT_TEIF 8
typedef struct {uint32_t CFGR,CNTR,MADDR;} dma_t;
typedef struct {int unused;} USART_TypeDef;
#define DMA_ARCH_TYPE dma_t
#define IS_DMA_ENABLED(d) (((dma_t *)(d))->CFGR & 1)
#define DMA_GET_FLAG_STATUS(d,f) ((d)->flags & (f))
#define DMA_CLEAR_FLAG(d,f) ((d)->flags &= ~(f))
typedef struct {void *USARTx;dma_t *txDMAResource;bool txDMAEmpty,txCompletePending;struct{uint8_t txBuffer[8];unsigned txBufferHead,txBufferTail,txBufferSize;}port;} uartPort_t;
typedef struct {uartPort_t port;} uartDevice_t;
typedef struct {uintptr_t userParam;uint32_t flags;} dmaChannelDescriptor_t;
static unsigned xDMA_GetCurrDataCounter(dma_t *d){return d->CNTR;}
static void xDMA_SetCurrDataCounter(dma_t *d,unsigned n){d->CNTR=n;}
static void xDMA_Cmd(dma_t *d,int enabled){d->CFGR=enabled;}
static void USART_ClearFlag(USART_TypeDef *u,int flag){(void)u;(void)flag;}
static void ch32DmaBarrier(void){}
static void uartTxMonitor(uartPort_t *p){}
'''
run('uart_tx_complete',pre+fn(uc,'void uartTryStartTxDMA(')+fn(uc,'static void uartTxMonitorIfComplete')+fn(uc,'static void handleUsartTxDma(')+fn(uc,'void uartDmaIrqHandler(')+'''
int main(void){dma_t reg={.CFGR=1,.CNTR=0};uartDevice_t d={.port={.txDMAResource=&reg,.port={.txBufferHead=3,.txBufferTail=1,.txBufferSize=8}}};dmaChannelDescriptor_t desc={.userParam=(uintptr_t)&d,.flags=DMA_IT_TCIF};uartDmaIrqHandler(&desc);printf("after TC: EN=%u CNTR=%u head=%u tail=%u (expected CNTR=2, tail=3)\\n",reg.CFGR,reg.CNTR,d.port.port.txBufferHead,d.port.port.txBufferTail);return reg.CNTR!=2;}
''')

ac='src/platform/CH32/adc_ch32h41x.c'
pre='''
#define USE_ADC_INTERNAL
#define USE_DMA_SPEC
#define ADCDEV_COUNT 2
#define ADCDEV_1 0
#define ADC_TAG_MAP_TEMPSENSOR 1
#define ADC_TAG_MAP_VREFINT 0
#define ADC_SampleTime_CyclesMode7 7
#define ARRAYLEN(x) (sizeof(x)/sizeof((x)[0]))
#define IO_CONFIG(a,b,c,d) 0
#define OWNER_ADC_BATT 1
#define OWNER_ADC 7
#define RESOURCE_INDEX(x) ((x)+1)
#define ENABLE 1
#define DISABLE 0
#define RCC_ADCCLKSource_HCLK 0
#define RCC_PPRE2_DIV0 0
#define RCC_HCLK_ADCPRE_DIV8 8
#define DMA_PERIPH_ADC 1
#define DMA_DIR_PeripheralSRC 0
#define DMA_PeripheralInc_Disable 0
#define DMA_MemoryInc_Enable 1
#define DMA_PeripheralDataSize_HalfWord 1
#define DMA_MemoryDataSize_Word 2
#define DMA_Mode_Circular 1
#define DMA_Priority_Low 0
#define DMA_M2M_Disable 0
#define VREFINT_CAL_VREF 3300
typedef enum {ADC_BATTERY,ADC_CURRENT,ADC_EXTERNAL1,ADC_RSSI,ADC_EXTERNAL_COUNT,ADC_TEMPSENSOR=ADC_EXTERNAL_COUNT,ADC_VREFINT,ADC_SOURCE_COUNT} adcSource_e;
typedef struct {int adcDevice,adcChannel,sampleTime,tag,dmaIndex;bool enabled;} adcOperatingConfig_t;
static adcOperatingConfig_t adcOperatingConfig[ADC_SOURCE_COUNT];
static uint32_t adcConversionBuffer[ADC_SOURCE_COUNT];
static int32_t adcVREFINTCAL;
typedef struct {uint32_t RDATAR,CTLR1;} ADC_TypeDef;
typedef struct {ADC_TypeDef *ADCx;int rccADC;} adcDevice_t;
static ADC_TypeDef regs[2];
static const adcDevice_t adcHardware[]={{&regs[0],1},{&regs[1],2}};
static struct {int channel;} adcTagMap[]={{17},{16}};
typedef struct {struct {bool enabled;int ioTag;}vbat,rssi,external1,current;int dmaopt[2];} adcConfig_t;
typedef struct {uint32_t DMA_PeripheralBaseAddr,DMA_Memory0BaseAddr,DMA_DIR,DMA_BufferSize,DMA_PeripheralInc,DMA_MemoryInc,DMA_PeripheralDataSize,DMA_MemoryDataSize,DMA_Mode,DMA_Priority,DMA_M2M;} DMA_InitTypeDef;
typedef struct {void *ref;int dmaMuxId;} dmaChannelSpec_t;
typedef int dmaIdentifier_e;
static int conversion_started;
static void setScalingFactors(void){adcVREFINTCAL=1501;}
static int adcFindTagMapEntry(int tag){return -1;}
static void *IOGetByTag(int tag){return NULL;}
static void IOInit(void *a,int b,int c){}
static void IOConfigGPIO(void *a,int b){}
static void RCC_ADCCLKConfig(int a){}
static void RCC_ADCHCLKCLKAsSourceConfig(int a,int b){}
static void RCC_ClockCmd(int a,int b){}
static void adcInitDevice(const adcDevice_t *a,int b){}
static const dmaChannelSpec_t *dmaGetChannelSpecByPeripheral(int a,int b,int opt){return NULL;}
static int dmaGetIdentifier(void *a){return 1;}
static bool dmaAllocate(int a,int b,int c){return false;}
static void dmaEnable(int a){}
static void xDMA_DeInit(void *a){}
static void xDMA_Init(void *a,DMA_InitTypeDef *b){}
static void dmaMuxEnable(int a,int b){}
static void xDMA_Cmd(void *a,int b){}
static void ADC_DMACmd(ADC_TypeDef *a,int b){}
static void ADC_RegularChannelConfig(ADC_TypeDef *a,int b,int c,int d){}
static void ADC_Cmd(ADC_TypeDef *a,int b){}
static void ADC_BufferCmd(ADC_TypeDef *a,int b){}
static void ADC_LowPowerModeCmd(ADC_TypeDef *a,int b){}
static void ADC_TempSensorVrefintCmd(int a){}
static void ADC_SoftwareStartConvCmd(ADC_TypeDef *a,int b){conversion_started++;}
'''
run('adc_no_dma',pre+fn(ac,'void adcInit(')+fn(ac,'bool adcInternalIsBusy(')+fn(ac,'uint16_t adcInternalRead(')+fn('src/main/drivers/adc.c','uint16_t adcInternalCompensateVref(')+'''
int main(void){setbuf(stdout,NULL);adcConfig_t c={.dmaopt={-1,-1}};adcInit(&c);uint16_t raw=adcInternalRead(ADC_VREFINT);printf("DMA opt=%d conversions started=%d vref enabled=%d busy=%d raw=%u\\n",c.dmaopt[0],conversion_started,adcOperatingConfig[ADC_VREFINT].enabled,adcInternalIsBusy(),raw);printf("vref=%u\\n",adcInternalCompensateVref(raw));}
''')

run('adc_internal_init_timeout', '''
#define ADC_INTERNAL_INIT_TIMEOUT_US 10000u
typedef uint32_t timeUs_t;
static uint32_t now, processed;
static bool unavailable;
static timeUs_t micros(void) { return now += 100; }
static bool adcInternalIsBusy(void) { return unavailable; }
static void adcInternalProcess(timeUs_t unused) { (void)unused; processed++; }
''' + fn('src/main/sensors/adcinternal.c','void adcInternalInit(') + '''
int main(void) {
    unavailable = true; adcInternalInit();
    if (processed || now > 10200) return 1;
    unavailable = false; now = 0; adcInternalInit();
    if (processed != 9) return 2;
    return 0;
}
''')

run('theia_async_font', '''
#define H415C_FONT_GLYPHS 512u
#define ARMED 1
#define ARMING_FLAG(x) (armed)
typedef uint32_t timeUs_t;
typedef struct { int unused; } displayPort_t;
typedef struct { uint8_t data[64]; } osdCharacter_t;
typedef enum { DISPLAY_FONT_ERROR, DISPLAY_FONT_DONE, DISPLAY_FONT_PENDING } displayFontResult_e;
typedef enum { THEIA_FONT_PENDING, THEIA_FONT_DONE, THEIA_FONT_ERROR } theiaFontStatus_e;
typedef struct { bool fontOutstanding; } theiaProducer_t;
static theiaProducer_t producer;
static int fontOwner;
static uint16_t fontGlyph;
static uint8_t fontData[54];
static timeUs_t fontStartedAt, now;
static bool armed;
static int writes, cancels;
static theiaFontStatus_e mockStatus;
static timeUs_t micros(void) { return now; }
static bool theiaProducerFontWrite(theiaProducer_t *p, uint16_t glyph, const uint8_t *data) {
    (void)glyph; (void)data; writes++; p->fontOutstanding = true; return true;
}
static theiaFontStatus_e theiaProducerFontStatus(theiaProducer_t *p) {
    if (mockStatus != THEIA_FONT_PENDING) p->fontOutstanding = false;
    return mockStatus;
}
static void theiaProducerFontCancel(theiaProducer_t *p) { p->fontOutstanding = false; cancels++; }
''' + fn('src/main/io/displayport_theia.c', 'static displayFontResult_e fontWrite(')
    + fn('src/main/io/displayport_theia.c', 'static void cancelFontWrite(') + '''
int main(void) {
    osdCharacter_t chr = {0};
    armed = true;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_ERROR || writes) return 1;
    armed = false; now = UINT32_MAX - 500000u;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_PENDING || writes != 1) return 2;
    if (fontWrite(NULL, 1, &chr, 4) != DISPLAY_FONT_ERROR || !producer.fontOutstanding) return 3;
    cancelFontWrite(NULL, 4);
    if (!producer.fontOutstanding || cancels) return 4;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_PENDING || writes != 1) return 5;
    now += 1000000u;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_ERROR || producer.fontOutstanding || cancels != 1) return 6;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_PENDING || writes != 2) return 7;
    mockStatus = THEIA_FONT_DONE;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_DONE || writes != 2) return 8;
    mockStatus = THEIA_FONT_PENDING;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_PENDING) return 9;
    armed = true;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_ERROR || cancels != 2) return 10;
    armed = false;
    if (fontWrite(NULL, 1, &chr, 3) != DISPLAY_FONT_PENDING) return 11;
    cancelFontWrite(NULL, 3);
    return producer.fontOutstanding || cancels != 3;
}
''')

run('theia_msp_osd_flags', '''
#define OSD_FLAGS_OSD_FEATURE (1 << 0)
#define OSD_FLAGS_OSD_HARDWARE_FRSKYOSD (1 << 3)
#define OSD_FLAGS_OSD_HARDWARE_MAX_7456 (1 << 4)
#define OSD_FLAGS_OSD_DEVICE_DETECTED (1 << 5)
#define OSD_FLAGS_OSD_MSP_DEVICE (1 << 6)
#define OSD_FLAGS_OSD_HARDWARE_AIRBOT_THEIA_OSD (1 << 7)
#define OSD_FLAGS_OSD_HARDWARE_FB_OSD (1 << 4)
#define ENABLE_FB_OSD 1
#define USE_MSP_DISPLAYPORT_FONT
typedef enum { OSD_DISPLAYPORT_DEVICE_NONE, OSD_DISPLAYPORT_DEVICE_THEIA,
 OSD_DISPLAYPORT_DEVICE_MAX7456, OSD_DISPLAYPORT_DEVICE_FRSKYOSD,
 OSD_DISPLAYPORT_DEVICE_MSP, OSD_DISPLAYPORT_DEVICE_FBOSD } osdDisplayPortDevice_e;
''' + fn('src/main/msp/msp.c', 'static uint8_t mspOsdFlags(') + '''
int main(void) {
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_THEIA, true) != 0x31) return 1;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_THEIA, false) != 0x11) return 2;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_NONE, true) != 0x01) return 3;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_MAX7456, true) != 0x31) return 4;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_FRSKYOSD, true) != 0x29) return 5;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_MSP, true) != 0xe1) return 6;
    if (mspOsdFlags(OSD_DISPLAYPORT_DEVICE_FBOSD, false) != 0x11) return 7;
    return 0;
}
''')

run('boot_button_sdi', '''
#define BOOT_SDI_HOLD_US 5000000u
#define GPIO_Pin_8 (1u << 8)
#define GPIO_Speed_Medium 1
#define GPIO_Mode_IPD 2
typedef uint32_t timeUs_t;
typedef struct { unsigned GPIO_Pin, GPIO_Speed, GPIO_Mode; } GPIO_InitTypeDef;
typedef struct { unsigned INDR; } gpio_t;
static gpio_t port;
#define GPIOC (&port)
static unsigned initCount, tasks, handoffs;
static void GPIO_Init(gpio_t *p, GPIO_InitTypeDef *g) {
    if (p != GPIOC || g->GPIO_Pin != GPIO_Pin_8 || g->GPIO_Mode != GPIO_Mode_IPD) abort();
    initCount++;
}
static void taskSystemLoad(timeUs_t t) { (void)t; tasks++; }
static void ch32h415UsbReleaseForDebug(void) { handoffs++; }
''' + fn('src/platform/CH32/target/CH32H415/boot_sdi.c', 'void ch32h415BootSdiSystemTask(') + '''
int main(void) {
    uint32_t t = UINT32_MAX - 1000u;
    ch32h415BootSdiSystemTask(t);
    if (initCount != 1 || handoffs) return 1;
    port.INDR = GPIO_Pin_8; ch32h415BootSdiSystemTask(t);
    t += 4999999u; ch32h415BootSdiSystemTask(t);
    if (handoffs) return 2;
    port.INDR = 0; ch32h415BootSdiSystemTask(t);
    port.INDR = GPIO_Pin_8; ch32h415BootSdiSystemTask(t);
    t += 5000000u; ch32h415BootSdiSystemTask(t);
    if (handoffs != 1 || initCount != 1) return 3;
    port.INDR = 0; ch32h415BootSdiSystemTask(t+10);
    port.INDR = GPIO_Pin_8; ch32h415BootSdiSystemTask(t+6000000u);
    return handoffs != 1 || tasks != 8;
}
''')

run('usb_sdi_handoff', '''
#define USBHS_IRQn 1
#define RCC_HBPeriph_USBHS 2
#define GPIO_Remap_SWJ_Disable 3
#define USBHS_UD_RST_LINK 1
#define USBHS_UD_RST_SIE 2
#define USBHS_UD_CLR_ALL 4
#define DISABLE 0
typedef struct { unsigned INT_EN, CONTROL; } usb_t;
static usb_t usb;
#define USBHSD (&usb)
static bool usbCoreInitialized, usbReleasedForDebug;
static unsigned irqDisabled, deinitialized, phyDisabled, remapped, detectDeinitialized;
static void NVIC_DisableIRQ(int n) { if (n != USBHS_IRQn) abort(); irqDisabled++; }
static void usbCableDetectDeinit(void) { if (!irqDisabled || !usbReleasedForDebug) abort(); detectDeinitialized++; }
static int usbd_deinitialize(int bus) { if (bus || !irqDisabled || !usbReleasedForDebug) abort(); deinitialized++; return 0; }
static void RCC_UTMIcmd(int on) { if (on || usb.INT_EN || usb.CONTROL != 7) abort(); phyDisabled++; }
static void GPIO_PinRemapConfig(int remap, int on) {
    if (remap != GPIO_Remap_SWJ_Disable || on || !phyDisabled) abort(); remapped++;
}
''' + fn('src/platform/CH32/serial_usb_vcp_ch32h4.c', 'void ch32h415UsbReleaseForDebug(')
       .replace('__asm volatile("fence iorw, iorw" ::: "memory");', '') + '''
int main(void) {
    usbCoreInitialized = true; usb.INT_EN = usb.CONTROL = 1;
    ch32h415UsbReleaseForDebug();
    if (!usbReleasedForDebug || usbCoreInitialized || deinitialized != 1 || remapped != 1) return 1;
    usbReleasedForDebug = false; usb.INT_EN = usb.CONTROL = 1;
    ch32h415UsbReleaseForDebug();
    return !usbReleasedForDebug || deinitialized != 1 || remapped != 2 || detectDeinitialized != 2;
}
''')

ic='src/platform/CH32/bus_i2c_ch32h41x.c'
pre='''
#define I2CINVALID -1
#define ENABLE 1
#define DISABLE 0
#define OWNER_I2C_SCL 1
#define OWNER_I2C_SDA 2
#define RESOURCE_INDEX(x) ((x)+1)
#define I2C_IT_EVT 1
#define I2C_IT_ERR 2
#define IOCFG_I2C_PU 1
#define IOCFG_I2C 0
#define I2C_Mode_I2C 0
#define I2C_DutyCycle_2 0
#define I2C_Ack_Enable 1
#define I2C_AcknowledgedAddress_7bit 0
#define NVIC_PRIO_I2C_ER 0
#define NVIC_PRIO_I2C_EV 0
typedef int i2cDevice_e;
typedef int *IO_t;
typedef int I2C_TypeDef;
typedef struct {I2C_TypeDef *reg;int rcc,er_irq,ev_irq;} i2cHardware_t;
typedef struct {const i2cHardware_t *hardware;IO_t scl,sda;struct {bool busy,error;}state;int pullUp,sclAF,sdaAF,clockSpeed;} i2cDevice_t;
static i2cDevice_t i2cDevice[1];
typedef struct {int I2C_Mode,I2C_DutyCycle,I2C_OwnAddress1,I2C_Ack,I2C_AcknowledgedAddress,I2C_ClockSpeed;} I2C_InitTypeDef;
static int recovery_attempts,i2cErrorCount;
static int IOGetOwner(IO_t io){return *io;}
static bool IOIsFreeOrPreinit(IO_t io){return *io==0;}
static void IOInit(IO_t io,int owner,int idx){*io=owner;}
static void RCC_ClockCmd(int a,int b){}
static void I2C_ITConfig(I2C_TypeDef *a,int b,int c){}
static void i2cUnstick(IO_t a,IO_t b){recovery_attempts++;}
static void IOConfigGPIOAF(IO_t a,int b,int c){}
static void I2C_DeInit(I2C_TypeDef *a){}
static void I2C_StructInit(I2C_InitTypeDef *a){}
static void I2C_Init(I2C_TypeDef *a,I2C_InitTypeDef *b){}
static void I2C_Cmd(I2C_TypeDef *a,int b){}
static void I2C_StretchClockCmd(I2C_TypeDef *a,int b){}
static void NVIC_SetPriority(int a,int b){}
static void NVIC_EnableIRQ(int a){}
'''
run('i2c_recovery',pre+fn(ic,'void i2cInit(')+fn(ic,'static bool i2cHandleHardwareFailure(')+'''
int main(void){int scl=0,sda=0,reg=0;i2cHardware_t hw={.reg=&reg};i2cDevice[0]=(i2cDevice_t){.hardware=&hw,.scl=&scl,.sda=&sda};i2cInit(0);recovery_attempts=0;i2cDevice[0].state.busy=true;i2cHandleHardwareFailure(0);printf("timeout recovery: unstick calls=%d busy=%d owners=%d/%d\\n",recovery_attempts,i2cDevice[0].state.busy,scl,sda);return recovery_attempts!=1;}
''')

pre='''
#define USE_PLATFORM_TIMER_FILTER
#define TIMER_CHANNEL_COUNT 3
typedef int ioTag_t;
typedef struct {int unused;} TIM_TypeDef;
typedef TIM_TypeDef timerResource_t;
typedef struct {ioTag_t tag;timerResource_t *tim;} timerHardware_t;
static TIM_TypeDef tim2,tim5,tim9,tim10,tim11,tim12;
#define TIM2 (&tim2)
#define TIM5 (&tim5)
#define TIM9 (&tim9)
#define TIM10 (&tim10)
#define TIM11 (&tim11)
#define TIM12 (&tim12)
#define TIMER_HARDWARE fullTimerHardware
static const timerHardware_t fullTimerHardware[]={{1,TIM2},{1,TIM5},{1,TIM9}};
static int ioToken;
static void *IOGetByTag(ioTag_t tag){return tag == 1 ? &ioToken : NULL;}
'''
run('timer_af_index',pre+fn('src/platform/CH32/timer_ch32h41x.c','bool timerPlatformIsAvailable(')+fn('src/main/drivers/timer_common.c','const timerHardware_t *timerGetByTagAndIndex')+'''
int main(void){
 const timerHardware_t *first=timerGetByTagAndIndex(1,1), *second=timerGetByTagAndIndex(1,2);
 if (!first || first->tim!=TIM5 || !second || second->tim!=TIM9) return 1;
 timerHardware_t hw={1,TIM2}; if (timerPlatformIsAvailable(&hw)) return 2;
 hw.tim=TIM10; if (timerPlatformIsAvailable(&hw)) return 3;
 hw.tim=TIM11; if (timerPlatformIsAvailable(&hw)) return 4;
 hw.tim=TIM12; if (timerPlatformIsAvailable(&hw)) return 5;
 hw.tim=TIM5; hw.tag=2; if (timerPlatformIsAvailable(&hw)) return 6;
 puts("Theia timers and unavailable pins remain excluded");
}
''')

pre='''
#define FAST_IRQ_HANDLER
#define DMA_IT_TCIF 2
#define DMA_IT_HTIF 4
#define DMA_IT_TEIF 8
#define USART_FLAG_TC 0x40
#define USART_STATR_ORE 0x08
#define USART_STATR_IDLE 0x10
#define USART_STATR_RXNE 0x20
#define USART_STATR_TC 0x40
#define USART_STATR_TXE 0x80
#define USART_CTLR1_IDLEIE 0x10
#define USART_CTLR1_RXNEIE 0x20
#define USART_CTLR1_TCIE 0x40
#define USART_CTLR1_TXEIE 0x80
#define CLEAR_BIT(reg,bit) ((reg)&=~(bit))
#define ENABLE 1
#define DISABLE 0
typedef struct {uint32_t CFGR,CNTR,MADDR;} dma_t;
typedef struct {uint16_t STATR,reserved0,DATAR,reserved1,BRR,reserved2,CTLR1,reserved3;} USART_TypeDef;
#define IS_DMA_ENABLED(d) (((dma_t *)(d))->CFGR & 1)
typedef struct {void *USARTx;dma_t *txDMAResource,*rxDMAResource;bool txDMAEmpty,txCompletePending;struct{uint8_t txBuffer[8];unsigned txBufferHead,txBufferTail,txBufferSize;uint8_t rxBuffer[8];unsigned rxBufferHead,rxBufferSize;void(*rxCallback)(uint32_t,void*);void*rxCallbackData;void(*idleCallback)(void);}port;} uartPort_t;
typedef struct {uartPort_t port;} uartDevice_t;
static unsigned monitorCalls,reads,received,receivedData,idleCalls,injectLate;
static void fakeClearTC(USART_TypeDef *u) { u->STATR &= ~USART_STATR_TC; if (injectLate) { u->STATR |= USART_STATR_RXNE; u->DATAR=0x5a; } }
static uint16_t fakeReadData(USART_TypeDef *u) { uint16_t data=u->DATAR; reads++; u->STATR &= ~(USART_STATR_IDLE|USART_STATR_RXNE|USART_STATR_ORE); return data; }
static void onRx(uint32_t data,void *ctx) { (void)ctx; received++; receivedData=data; }
static void onIdle(void) { idleCalls++; }
static void uartTxMonitor(uartPort_t *p){(void)p;monitorCalls++;}
'''
uart_irq = fn('src/platform/CH32/serial_uart_ch32bsp.c','FAST_IRQ_HANDLER void uartIrqHandler').replace(
    'USARTx->STATR = (uint16_t)~USART_STATR_TC;', 'fakeClearTC(USARTx);').replace(
    'const uint16_t data = USARTx->DATAR;', 'const uint16_t data = fakeReadData(USARTx);')
uart_rx = fn('src/platform/CH32/serial_uart_ch32bsp.c','static inline void uartStoreRxByte')
run('uart_idle_registration', '''
#include <stddef.h>
#define USE_PLATFORM_UART_IDLE_CALLBACK
#define NVIC_PRIO_SERIALUART1 0
#define ATOMIC_BLOCK(x) for (int once=1; once; once=0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define USART_IT_IDLE 0x10
#define ENABLE 1
#define DISABLE 0
#define MODE_RX 1
typedef void (*serialIdleCallbackPtr)(void);
typedef struct serialPort_s serialPort_t;
struct vtable { void (*setIdleCallback)(serialPort_t *, serialIdleCallbackPtr); };
struct serialPort_s { const struct vtable *vTable; serialIdleCallbackPtr idleCallback; int mode; };
typedef struct { uint32_t CTLR1; } USART_TypeDef;
typedef struct { serialPort_t port; void *USARTx, *rxDMAResource; } uartPort_t;
static void USART_ITConfig(USART_TypeDef *u, int flag, int enabled) { if (enabled) u->CTLR1 |= flag; else u->CTLR1 &= ~flag; }
''' + fn('src/platform/CH32/serial_uart_ch32bsp.c','void uartSetIdleCallback(')
    + fn('src/main/drivers/serial.c','void serialSetIdleCallback(') + '''
static void cb(void) {}
int main(void) {
    USART_TypeDef u={0}; static const struct vtable vt={uartSetIdleCallback}, plain={NULL};
    uartPort_t p={.port={.vTable=&vt,.mode=MODE_RX},.USARTx=&u};
    serialSetIdleCallback(&p.port,cb);
    if (p.port.idleCallback!=cb || !(u.CTLR1 & USART_IT_IDLE)) return 1;
    serialSetIdleCallback(&p.port,NULL);
    if (p.port.idleCallback || (u.CTLR1 & USART_IT_IDLE)) return 2;
    p.rxDMAResource=&u;serialSetIdleCallback(&p.port,cb);
    if (u.CTLR1 & USART_IT_IDLE) return 3;
    p.port.vTable=&plain;serialSetIdleCallback(&p.port,NULL);
    if (p.port.idleCallback) return 4;
    puts("callbacks registered after open enable IDLE without reconfiguring the UART");
}
''')
run('uart_line_end_release',pre+uart_rx+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void uartTxMonitorIfComplete')+uart_irq+'''
int main(void){USART_TypeDef usart={.CTLR1=USART_CTLR1_TCIE};uartDevice_t d={.port={.USARTx=&usart,.txDMAResource=NULL,.rxDMAResource=NULL,.port={.txBufferHead=2,.txBufferTail=1,.txBufferSize=8}}};
 usart.STATR=USART_STATR_TC;uartIrqHandler(&d.port);if(monitorCalls)return 1;
 d.port.port.txBufferTail=d.port.port.txBufferHead;usart.STATR=USART_STATR_TC;uartIrqHandler(&d.port);return monitorCalls!=1;}
''')
run('uart_full_duplex_rx',pre+uart_rx+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void uartTxMonitorIfComplete')+uart_irq+'''
int main(void) {
 USART_TypeDef u={.STATR=USART_STATR_TC|USART_STATR_IDLE,.CTLR1=USART_CTLR1_TCIE};
 uartPort_t s={.USARTx=&u,.port={.rxCallback=onRx,.idleCallback=onIdle,.txBufferSize=8,.rxBufferSize=8}};
 injectLate=1; uartIrqHandler(&s);
 if (reads || received || idleCalls || !(u.STATR & USART_STATR_RXNE)) return 1;
 injectLate=0; uartIrqHandler(&s);
 if (received!=1 || receivedData!=0x5a || idleCalls) return 2;
 received=reads=0; u.STATR=USART_STATR_TC|USART_STATR_IDLE;u.CTLR1=USART_CTLR1_TCIE|USART_CTLR1_IDLEIE;
 injectLate=1; uartIrqHandler(&s);
 if (received!=1 || receivedData!=0x5a || idleCalls!=1 || reads!=1) return 3;
 received=reads=0;injectLate=0;u.STATR=USART_STATR_RXNE|USART_STATR_ORE|USART_STATR_TC|USART_STATR_TXE;u.CTLR1=USART_CTLR1_RXNEIE;u.DATAR=0x37;
 uartIrqHandler(&s);
 if (received!=1 || receivedData!=0x37 || reads!=1 || !(u.STATR & USART_STATR_TC) || !(u.STATR & USART_STATR_TXE)) return 4;
 puts("disabled flags do not trigger TX or IDLE; late RX and RXNE+ORE are delivered");
}
''')
pre += '''
#define NVIC_PRIO_SERIALUART_TXDMA 0x50
#define ATOMIC_BLOCK(x) for(int once=1;once;once=0)
#define DMA_ARCH_TYPE dma_t
#define DMA_GET_FLAG_STATUS(d,f) ((d)->flags & (f))
#define DMA_CLEAR_FLAG(d,f) ((d)->flags &= ~(f))
typedef struct {uintptr_t userParam;uint32_t flags;} dmaChannelDescriptor_t;
static unsigned xDMA_GetCurrDataCounter(dma_t *d){return d->CNTR;}
static void USART_ClearFlag(USART_TypeDef *u, int flag){u->STATR &= ~flag;}
static void xDMA_SetCurrDataCounter(dma_t *d,unsigned n){d->CNTR=n;}
static void xDMA_Cmd(dma_t *d,int enabled){d->CFGR=enabled;}
static void ch32DmaBarrier(void){}
'''
run('uart_usart_before_dma',pre+fn('src/platform/CH32/serial_uart_ch32bsp.c','void uartTryStartTxDMA(')+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void uartTxMonitorIfComplete')+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void handleUsartTxDma(')+fn('src/platform/CH32/serial_uart_ch32bsp.c','void uartDmaIrqHandler(')+uart_rx+uart_irq+'''
int main(void){USART_TypeDef usart={.CTLR1=USART_CTLR1_TCIE};dma_t reg={.CFGR=1,.CNTR=0};uartDevice_t d={.port={.USARTx=&usart,.txDMAResource=&reg,.rxDMAResource=NULL,.port={.txBufferHead=2,.txBufferTail=2,.txBufferSize=8}}};
 usart.STATR=USART_STATR_TC;uartIrqHandler(&d.port);if(monitorCalls||!d.port.txCompletePending)return 1;
 dmaChannelDescriptor_t desc={.userParam=(uintptr_t)&d,.flags=DMA_IT_TCIF};uartDmaIrqHandler(&desc);
 return reg.CFGR!=0||monitorCalls!=1||d.port.txCompletePending;}
''')

dma_hdr = (root/'lib/main/CH32H41x/Peripheral/inc/ch32h417_dma.h').read_text()
dma_enum = dma_hdr[dma_hdr.index('typedef enum'):dma_hdr.index('} dmamux_requst_id_sel_type;')+len('} dmamux_requst_id_sel_type;')]
reqmap_txt = (root/'src/platform/CH32/dma_reqmap_mcu.c').read_text()
reqmap_body = reqmap_txt[reqmap_txt.index('typedef struct dmaPeripheralMapping_s'):reqmap_txt.index('const dmaChannelSpec_t *dmaGetChannelSpecByTimer(')]
timer_def_txt = (root/'src/platform/CH32/timer_def.h').read_text()
timer_def_body = timer_def_txt[timer_def_txt.index('#ifdef USE_THEIA_OSD'):timer_def_txt.index('// AF table for timer')]
reqmap_common = '''
#include "''' + str(root/'src/main/common/utils.h') + '''"
#include "''' + str(root/'src/platform/CH32/dma_reqmap_mcu.h') + '''"
''' + dma_enum + '''
typedef struct dmaResource_s { int dummy; } dmaResource_t;
typedef struct timerResource_s { int dummy; } timerResource_t;
typedef uint8_t ioTag_t;
typedef uint16_t dmaCode_t;
typedef int8_t dmaoptValue_t;
#define DMA_OPT_UNUSED (-1)
typedef struct dmaChannelSpec_s {
    dmaCode_t code;
    dmaResource_t *ref;
    uint32_t dmaMuxId;
} dmaChannelSpec_t;
#define DMA_CODE(dma, stream, chanreq) (((dma) << 12) | ((stream) << 8) | ((chanreq) << 0))
#define DMA_CODE_CONTROLLER(code) (((code) >> 12) & 0xf)
#define DMA_CODE_STREAM(code) (((code) >> 8) & 0xf)
typedef enum {
    DMA_PERIPH_SPI_SDO,
    DMA_PERIPH_SPI_SDI,
    DMA_PERIPH_ADC,
    DMA_PERIPH_SDIO,
    DMA_PERIPH_UART_TX,
    DMA_PERIPH_UART_RX,
    DMA_PERIPH_TIMUP,
} dmaPeripheral_e;
#define TIM_Channel_1 0x00
#define TIM_Channel_2 0x04
#define TIM_Channel_3 0x08
#define TIM_Channel_4 0x0C
#define TIMER_OUTPUT_NONE 0
#define TIMER_OUTPUT_N_CHANNEL 1
static timerResource_t t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,t11,t12;
#define TIM1 (&t1)
#define TIM2 (&t2)
#define TIM3 (&t3)
#define TIM4 (&t4)
#define TIM5 (&t5)
#define TIM6 (&t6)
#define TIM7 (&t7)
#define TIM8 (&t8)
#define TIM9 (&t9)
#define TIM10 (&t10)
#define TIM11 (&t11)
#define TIM12 (&t12)
static dmaResource_t d1[8], d2[8];
#define DMA1_Channel1 (&d1[0])
#define DMA1_Channel2 (&d1[1])
#define DMA1_Channel3 (&d1[2])
#define DMA1_Channel4 (&d1[3])
#define DMA1_Channel5 (&d1[4])
#define DMA1_Channel6 (&d1[5])
#define DMA1_Channel7 (&d1[6])
#define DMA1_Channel8 (&d1[7])
#define DMA2_Channel1 (&d2[0])
#define DMA2_Channel2 (&d2[1])
#define DMA2_Channel3 (&d2[2])
#define DMA2_Channel4 (&d2[3])
#define DMA2_Channel5 (&d2[4])
#define DMA2_Channel6 (&d2[5])
#define DMA2_Channel7 (&d2[6])
#define DMA2_Channel8 (&d2[7])
#define USE_TIMER
#define USE_SPI
#define USE_ADC
#define SPIDEV_1 0
#define SPIDEV_2 1
#define SPIDEV_3 2
#define SPIDEV_4 3
#define ADCDEV_1 0
#define ADCDEV_2 1
#define TIM_N(n) (1U << (n))
''' + timer_def_body + '''
#define TIMER_INDEX(i) BITCOUNT((TIM_N(i) - 1) & USED_TIMERS)
''' + reqmap_body

run('dma_reqmap_theia_osd', '#define USE_THEIA_OSD\n' + reqmap_common + '''
int main(void) {
    if (MAX_TIMER_DMA_OPTIONS != 16 || MAX_PERIPHERAL_DMA_OPTIONS != 16) return 1;
    if (DEF_TIM_DMA_REQ__BTCH_TIM6_UP != DMAMUX_DMAREQ_ID_TIM6_UP) return 2;
    if (DEF_TIM_DMA_REQ__BTCH_TIM7_UP != DMAMUX_DMAREQ_ID_TIM7_UP) return 3;
    const dmaChannelSpec_t *s;
    if (TIMER_INDEX(3) != 1 || TIMER_INDEX(9) != 7) return 4;
    s = dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(3), 0);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM3_UP || s->ref != DMA1_Channel1) return 5;
    s = dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(6), 8);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM6_UP || s->ref != DMA2_Channel1) return 6;
    s = dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(7), 9);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM7_UP || s->ref != DMA2_Channel2) return 7;
    s = dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(9), 15);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM9_UP || s->ref != DMA2_Channel8) return 8;
    if (dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, 8, 0) != NULL) return 9;
    if (dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(3), 13) != NULL) return 10;
    if (dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(3), 16) != NULL) return 11;
    s = dmaGetChannelSpecByTimerValue(TIM9, TIM_Channel_1, 0);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM9_CH1) return 12;
    s = dmaGetChannelSpecByTimerValue(TIM9, TIM_Channel_4, 15);
    if (!s || s->dmaMuxId != DMAMUX_DMAREQ_ID_TIM9_CH4 || s->ref != DMA2_Channel8) return 13;
    if (dmaGetChannelSpecByTimerValue(TIM2, TIM_Channel_1, 0) != NULL) return 14;
    if (dmaGetChannelSpecByTimerValue(TIM10, TIM_Channel_1, 0) != NULL) return 15;
    if (dmaGetChannelSpecByTimerValue(TIM11, TIM_Channel_2, 0) != NULL) return 16;
    if (dmaGetChannelSpecByTimerValue(TIM12, TIM_Channel_4, 0) != NULL) return 17;
    if (dmaGetChannelSpecByTimerValue(TIM9, TIM_Channel_1, 13) != NULL) return 18;
    if (dmaGetChannelSpecByTimerValue(TIM9, TIM_Channel_1, 16) != NULL) return 19;
    puts("OSD mode preserves TIM3_UP/TIM6_UP/TIM7_UP/TIM9_UP and TIM9 CC while isolating OSD timers and DMA2_CH6");
}
''')

run('dma_reqmap_full_no_osd', reqmap_common + '''
int main(void) {
    timerResource_t *tims[9] = {TIM1, TIM2, TIM3, TIM4, TIM5, TIM8, TIM9, TIM10, TIM11};
    uint32_t ch1_reqs[9] = {
        DMAMUX_DMAREQ_ID_TIM1_CH1, DMAMUX_DMAREQ_ID_TIM2_CH1, DMAMUX_DMAREQ_ID_TIM3_CH1,
        DMAMUX_DMAREQ_ID_TIM4_CH1, DMAMUX_DMAREQ_ID_TIM5_CH1, DMAMUX_DMAREQ_ID_TIM8_CH1,
        DMAMUX_DMAREQ_ID_TIM9_CH1, DMAMUX_DMAREQ_ID_TIM10_CH1, DMAMUX_DMAREQ_ID_TIM11_CH1,
    };
    for (unsigned i = 0; i < 9; i++) {
        const dmaChannelSpec_t *s = dmaGetChannelSpecByTimerValue(tims[i], TIM_Channel_1, 13);
        if (!s || s->dmaMuxId != ch1_reqs[i]) return 1 + i;
    }
    const dmaChannelSpec_t *s12 = dmaGetChannelSpecByTimerValue(TIM12, TIM_Channel_4, 15);
    if (!s12 || s12->dmaMuxId != DMAMUX_DMAREQ_ID_TIM12_CH4) return 20;
    uint32_t up_reqs[12] = {
        DMAMUX_DMAREQ_ID_TIM1_UP, DMAMUX_DMAREQ_ID_TIM2_UP, DMAMUX_DMAREQ_ID_TIM3_UP,
        DMAMUX_DMAREQ_ID_TIM4_UP, DMAMUX_DMAREQ_ID_TIM5_UP, DMAMUX_DMAREQ_ID_TIM6_UP,
        DMAMUX_DMAREQ_ID_TIM7_UP, DMAMUX_DMAREQ_ID_TIM8_UP, DMAMUX_DMAREQ_ID_TIM9_UP,
        DMAMUX_DMAREQ_ID_TIM10_UP, DMAMUX_DMAREQ_ID_TIM11_UP, DMAMUX_DMAREQ_ID_TIM12_UP,
    };
    for (unsigned n = 1; n <= 12; n++) {
        const dmaChannelSpec_t *su = dmaGetChannelSpecByPeripheral(DMA_PERIPH_TIMUP, TIMER_INDEX(n), 0);
        if (!su || su->dmaMuxId != up_reqs[n - 1]) return 30 + n;
    }
    puts("non-OSD mode exposes all TIM1..12 CC and TIM_UP DMAMUX mappings");
}
''')

run('dma_mux_enable_bounds_and_clocks', dma_enum + '''
typedef int dmaIdentifier_e;
#define DMA_NONE 0
#define DMA_FIRST_HANDLER 1
#define DMA_LAST_HANDLER 16
#define DMA_IDENTIFIER_TO_INDEX(x) ((x) - DMA_FIRST_HANDLER)
#define ENABLE 1
#define RCC_HB_DMA1 0x01u
#define RCC_HB_DMA2 0x02u
#define RCC_HB(periph) RCC_HB_ ## periph
typedef struct { uint32_t CFGR0_3, CFGR4_7, CFGR8_11, CFGR12_15; } DMAMUX_TypeDef;
static DMAMUX_TypeDef mux_hw;
#define DMAMUX (&mux_hw)
static int dma1_token, dma2_token;
#define DMA1 (&dma1_token)
#define DMA2 (&dma2_token)
typedef void (*dmaCallbackHandlerFuncPtr)(void *);
typedef struct { void *dma; dmaCallbackHandlerFuncPtr irqHandlerCallback; int32_t irqN; uint32_t userParam; } dmaChannelDescriptor_t;
static dmaChannelDescriptor_t dmaDescriptors[DMA_LAST_HANDLER];
static uint32_t enabled_clocks;
static void RCC_ClockCmd(uint32_t periph, int state) { if (state) enabled_clocks |= periph; }
static void NVIC_SetPriority(int32_t irq, uint32_t prio) { (void)irq; (void)prio; }
static void NVIC_EnableIRQ(int32_t irq) { (void)irq; }
#define CH32_DMAMUX_MAX_REQUEST_ID DMAMUX_DMAREQ_ID_TIM7_UP
''' + fn('lib/main/CH32H41x/Peripheral/src/ch32h417_dma.c', 'void DMA_MuxChannelConfig(')
    + fn('src/platform/CH32/dma_ch32h41x.c', 'static void enableDmaClock(')
    + fn('src/platform/CH32/dma_ch32h41x.c', 'void dmaEnable(')
    + fn('src/platform/CH32/dma_ch32h41x.c', 'void dmaMuxEnable(')
    + fn('src/platform/CH32/dma_ch32h41x.c', 'void dmaSetHandler(') + '''
int main(void) {
    for (int i = 0; i < 8; i++) dmaDescriptors[i].dma = DMA1;
    for (int i = 8; i < 16; i++) dmaDescriptors[i].dma = DMA2;
    mux_hw.CFGR0_3 = 0x55555555u;
    dmaMuxEnable(DMA_NONE, DMAMUX_DMAREQ_ID_TIM1_CH1);
    dmaMuxEnable(DMA_LAST_HANDLER + 1, DMAMUX_DMAREQ_ID_TIM1_CH1);
    dmaMuxEnable(1, 0);
    dmaMuxEnable(1, 0x7Cu);
    dmaEnable(DMA_NONE);
    dmaEnable(DMA_LAST_HANDLER + 1);
    dmaSetHandler(DMA_NONE, NULL, 0, 0);
    dmaSetHandler(DMA_LAST_HANDLER + 1, NULL, 0, 0);
    if (enabled_clocks != 0 || mux_hw.CFGR0_3 != 0x55555555u) return 1;
    dmaMuxEnable(9, DMAMUX_DMAREQ_ID_TIM9_CH1);
    if (enabled_clocks != (RCC_HB_DMA1 | RCC_HB_DMA2)) return 2;
    if ((mux_hw.CFGR8_11 & 0xFFu) != (DMAMUX_DMAREQ_ID_TIM9_CH1 - 1u)) return 3;
    dmaMuxEnable(16, DMAMUX_DMAREQ_ID_TIM7_UP);
    if (((mux_hw.CFGR12_15 >> 24) & 0xFFu) != (DMAMUX_DMAREQ_ID_TIM7_UP - 1u)) return 4;
    puts("dmaMuxEnable guards bounds/zero-underflow and enables DMAMUX + controller HB clocks");
}
''')

bb_txt = (root/'src/platform/CH32/dshot_bitbang.c').read_text()
bb_hw_table = bb_txt[bb_txt.index('const timerHardware_t bbTimerHardware[]'):bb_txt.index(';', bb_txt.index('const timerHardware_t bbTimerHardware[]'))+1]

run('dshot_bb_multiport_dmamux', '''
#define USE_DMA_SPEC
#define USE_PLATFORM_DMA_FILTER
#define MAX_PERIPHERAL_DMA_OPTIONS 16
#define MAX_SUPPORTED_MOTOR_PORTS 4
#define ARRAYLEN(x) (sizeof(x)/sizeof((x)[0]))
#define CC_CHANNELS_PER_TIMER 4
#define CC_CHANNEL_FROM_INDEX(x) ((x)*4)
#define DSHOT_BITBANGED_TIMER_AUTO 0
#define DSHOT_BITBANGED_TIMER_TIM1 1
#define DSHOT_BITBANGED_TIMER_TIM8 2
#define OWNER_FREE 0
#define OWNER_DSHOT_BITBANG 1
#define OWNER_SPI 2
#define TIM1 1
#define TIM8 8
#define NONE 0
#define CH1 0
#define CH2 4
#define CH3 8
#define CH4 12
typedef int resourceOwner_e;
typedef int8_t dmaoptValue_t;
typedef int dmaIdentifier_e;
typedef struct { int idx; } dmaResource_t;
typedef struct { int tim; uint8_t channel; int8_t opt; } timerHardware_t;
#define DEF_TIM(t,ch,tag,out,dma,up) { t, ch, dma }
''' + bb_hw_table + '''
typedef struct { int owner; } resourceOwner_t;
typedef struct { dmaResource_t *ref; uint32_t dmaMuxId; } dmaChannelSpec_t;
static struct { const timerHardware_t *timhw; dmaResource_t *dmaResource; uint32_t dmaChannel; } bbPorts[4];
static dmaResource_t dmas[16];
static resourceOwner_t dmaOwners[16];
static resourceOwner_t freeOwner = { OWNER_FREE };
static struct { struct { int useDshotBitbangedTimer; } dev; } cfg;
#define motorConfig() (&cfg)
static int lookupTimerIndex(int tim) { return tim; }
static int timerGetNumberByIndex(int idx) { return idx; }
static const timerHardware_t *timerGetAllocatedByNumberAndChannel(int t, int c) { (void)t; (void)c; return NULL; }
static const resourceOwner_t *timerGetOwner(const timerHardware_t *t) { (void)t; return &freeOwner; }
static dmaoptValue_t dmaGetOptionByTimer(const timerHardware_t *t) { return t->opt; }
static bool dmaIsReserved(dmaIdentifier_e id) { return id == 14; } /* DMA2_CH6 -> opt 13 */
static dmaIdentifier_e dmaGetIdentifier(const dmaResource_t *d) { return d->idx + 1; }
static const resourceOwner_t *dmaGetOwner(dmaIdentifier_e id) { return &dmaOwners[id - 1]; }
static dmaChannelSpec_t specBuf;
static const dmaChannelSpec_t *dmaGetChannelSpecByTimerValue(int tim, uint8_t ch, dmaoptValue_t opt) {
    if (opt < 0 || opt >= 16 || opt == 13) return NULL;
    specBuf.ref = &dmas[opt];
    specBuf.dmaMuxId = (tim == TIM8 ? 32u : 1u) + (ch / 4u);
    return &specBuf;
}
''' + fn('src/platform/CH32/timer_ch32bsp.c', 'int8_t timerGetTIMNumber(')
    + fn('src/platform/CH32/dshot_bitbang.c', 'static void bbFindPacerTimer(void)') + '''
int main(void) {
    for (int i = 0; i < 16; i++) { dmas[i].idx = i; dmaOwners[i].owner = OWNER_FREE; }
    /* Occupy DMA2_Ch1..4 (8..11) and DMA2_Ch7..8 (14..15); DMA2_Ch6 (13) is OSD-reserved.
     * Only DMA2_Ch5 (12) and DMA1_Ch1..8 (0..7) remain free! */
    for (int i = 8; i <= 11; i++) dmaOwners[i].owner = OWNER_SPI;
    dmaOwners[14].owner = OWNER_SPI;
    dmaOwners[15].owner = OWNER_SPI;

    cfg.dev.useDshotBitbangedTimer = DSHOT_BITBANGED_TIMER_AUTO;
    bbFindPacerTimer();
    /* AUTO mode interleaves TIM8, TIM1, TIM8, TIM1 with 4 unique non-reserved DMAMUX channels */
    int expectedTim[4] = { TIM8, TIM1, TIM8, TIM1 };
    uint8_t expectedCh[4] = { CH1, CH1, CH2, CH2 };
    int expectedOpt[4] = { 12, 0, 1, 2 };
    uint32_t expectedMux[4] = { 32, 1, 33, 2 };
    for (int p = 0; p < 4; p++) {
        if (!bbPorts[p].timhw || bbPorts[p].timhw->tim != expectedTim[p] || bbPorts[p].timhw->channel != expectedCh[p]) return 1 + p;
        if (bbPorts[p].dmaResource != &dmas[expectedOpt[p]] || bbPorts[p].dmaChannel != expectedMux[p]) return 10 + p;
    }

    /* Forced TIM1 mode assigns TIM1 CH1..4 across 4 unique DMAMUX channels */
    cfg.dev.useDshotBitbangedTimer = DSHOT_BITBANGED_TIMER_TIM1;
    bbFindPacerTimer();
    for (int p = 0; p < 4; p++) {
        if (!bbPorts[p].timhw || bbPorts[p].timhw->tim != TIM1 || bbPorts[p].timhw->channel != p * 4) return 20 + p;
        for (int q = 0; q < p; q++) {
            if (bbPorts[p].dmaResource == bbPorts[q].dmaResource) return 30 + p;
        }
    }
    puts("multi-port bitbang interleaves TIM8/TIM1 and dynamically allocates collision-free DMAMUX channels across DMA2/DMA1");
}
''')

run('dshot_bb_shared_pacer_bidir_sync', '''
#define USE_DSHOT_TELEMETRY
#define DEBUG_COUNT_INTERRUPT
#define DISABLE 0
#define ENABLE 1
#define DSHOT_BITBANG_DIRECTION_OUTPUT 0
#define DSHOT_BITBANG_DIRECTION_INPUT 1
#define DSHOT_BITBANG_STATUS_OK 0
#define DSHOT_BITBANG_STATUS_DMA_ERROR 3
#define DMA_IT_TCIF 0x02u
#define DMA_IT_HTIF 0x04u
#define DMA_IT_TEIF 0x08u
#define BB_DMA_FLAGS (DMA_IT_TCIF | DMA_IT_HTIF | DMA_IT_TEIF)
#define TIM_PSCReloadMode_Immediate 0x01u
#define TIM_FLAG_Update 0x01u
#define FAST_IRQ_HANDLER
#define FAST_CODE
#define dbgPinHi(x) ((void)0)
#define dbgPinLo(x) ((void)0)
typedef uint32_t timeUs_t;
typedef struct { uint32_t ATRLR, CNT, SWEVGR, INTFR, DMAINTENR; unsigned ugCount; uint32_t shadowATRLR; } TIM_TypeDef;
typedef struct { TIM_TypeDef *tim; } timerHardware_t;
typedef struct { uint32_t CFGR; } dmaResource_t;
typedef struct { TIM_TypeDef *tim; uint16_t dmaSources, armedOutputSources; volatile uint16_t pendingOutputSources, pendingInputSources; uint16_t outputARR, inputARR; } bbPacer_t;
typedef struct {
    bbPacer_t *pacer;
    timerHardware_t *timhw;
    dmaResource_t *dmaResource;
    uint16_t dmaSource, outputARR, inputARR;
    uint8_t direction;
    volatile bool telemetryPending, reinitRequired;
    bool inputActive, telemetryAborted;
    uint32_t outputIrq, inputIrq, errorIrq;
} bbPort_t;
typedef struct { uintptr_t userParam; uint32_t flags; } dmaChannelDescriptor_t;
#define DMA_GET_FLAG_STATUS(d, f) ((d)->flags & (f))
#define DMA_CLEAR_FLAG(d, f) ((d)->flags &= ~(f))
static bbPacer_t bbPacers[1];
static bbPort_t bbPorts[2];
static int usedMotorPacers = 1, usedMotorPorts = 2, dshotMotorCount = 4, bbStatus;
static bool useDshotTelemetry = true;
static timeUs_t lastSendUs;
static timeUs_t micros(void) { return 100; }
static bool dshotCommandQueueEmpty(void) { return true; }
static bool dshotCommandOutputIsEnabled(int n) { (void)n; return true; }
static void bbDMA_Cmd(bbPort_t *p, int state) { p->dmaResource->CFGR = (uint32_t)state; }
static void bbTIM_ReloadPeriod(void *TIMx, uint16_t period) {
    TIM_TypeDef *tim = (TIM_TypeDef *)TIMx;
    tim->ATRLR = period;
    tim->SWEVGR = TIM_PSCReloadMode_Immediate;
    tim->shadowATRLR = tim->ATRLR;
    tim->CNT = 0;
    tim->ugCount++;
    tim->INTFR = (uint16_t)~TIM_FLAG_Update;
}
static void bbTIM_DMACmd(void *TIMx, uint16_t src, int state) {
    TIM_TypeDef *tim = (TIM_TypeDef *)TIMx;
    if (state) tim->DMAINTENR |= src;
    else tim->DMAINTENR &= (uint16_t)~src;
}
static bool bbSwitchToOutput(bbPort_t *p) {
    p->timhw->tim->ATRLR = p->outputARR;
    p->direction = DSHOT_BITBANG_DIRECTION_OUTPUT;
    return true;
}
static void bbSwitchToInput(bbPort_t *p) {
    bbDMA_Cmd(p, ENABLE);
    p->direction = DSHOT_BITBANG_DIRECTION_INPUT;
}
''' + fn('src/platform/common/stm32/dshot_bitbang_impl.h', 'static inline bool bbDMAServiceFlags(')
    + fn('src/platform/CH32/dshot_bitbang.c', 'FAST_IRQ_HANDLER void bbDMAIrqHandler(')
    + fn('src/platform/CH32/dshot_bitbang.c', 'static void bbUpdateComplete(void)') + '''
int main(void) {
    TIM_TypeDef tim1 = { .ATRLR = 154, .shadowATRLR = 154, .CNT = 77 };
    timerHardware_t hw0 = { &tim1 }, hw1 = { &tim1 };
    dmaResource_t dma0 = {0}, dma1 = {0};
    bbPacers[0] = (bbPacer_t){ .tim = &tim1, .dmaSources = 0x06u };
    bbPorts[0] = (bbPort_t){ .pacer = &bbPacers[0], .timhw = &hw0, .dmaResource = &dma0, .dmaSource = 0x02u, .outputARR = 193, .inputARR = 154, .direction = DSHOT_BITBANG_DIRECTION_INPUT };
    bbPorts[1] = (bbPort_t){ .pacer = &bbPacers[0], .timhw = &hw1, .dmaResource = &dma1, .dmaSource = 0x04u, .outputARR = 193, .inputARR = 154, .direction = DSHOT_BITBANG_DIRECTION_INPUT };

    /* 1. Normal frame: output reload happens once with shadowATRLR == 193 and both CC1|CC2 armed */
    bbUpdateComplete();
    if (tim1.shadowATRLR != 193 || tim1.ugCount != 1 || tim1.DMAINTENR != 0x06u) return 1;
    tim1.CNT = 120;

    /* Port 0 finishes output first: must NOT reset CNT or reload inputARR while Port 1 is still outputting */
    dmaChannelDescriptor_t desc0 = { .userParam = (uintptr_t)&bbPorts[0], .flags = DMA_IT_TCIF };
    bbDMAIrqHandler(&desc0);
    if (tim1.CNT != 120 || tim1.shadowATRLR != 193 || tim1.ugCount != 1 || tim1.DMAINTENR != 0x04u) return 2;
    if (!bbPorts[0].telemetryPending || bbPorts[0].direction != DSHOT_BITBANG_DIRECTION_INPUT || dma0.CFGR != 1) return 3;

    /* Port 1 finishes output: single synchronized UG switches shadowATRLR -> 154, CNT -> 0, and enables CC1|CC2 input */
    dmaChannelDescriptor_t desc1 = { .userParam = (uintptr_t)&bbPorts[1], .flags = DMA_IT_TCIF };
    bbDMAIrqHandler(&desc1);
    if (tim1.CNT != 0 || tim1.shadowATRLR != 154 || tim1.ugCount != 2 || tim1.DMAINTENR != 0x06u) return 4;

    /* Both ports complete input reception cleanly */
    desc0.flags = DMA_IT_TCIF; bbDMAIrqHandler(&desc0);
    desc1.flags = DMA_IT_TCIF; bbDMAIrqHandler(&desc1);
    if (bbPorts[0].telemetryPending || bbPorts[1].telemetryPending || tim1.DMAINTENR != 0) return 5;

    /* 2. Fault isolation: if Port 1 hits TEIF during output after Port 0 armed input, Port 0 still receives input clocks */
    bbUpdateComplete();
    desc0.flags = DMA_IT_TCIF; bbDMAIrqHandler(&desc0);
    desc1.flags = DMA_IT_TEIF; bbDMAIrqHandler(&desc1);
    if (!bbPorts[1].reinitRequired || tim1.shadowATRLR != 154 || tim1.DMAINTENR != 0x02u) return 6;
    puts("shared pacer synchronizes ARPE shadow reload once across multi-port output->input transitions and isolates TEIF");
}
''')
