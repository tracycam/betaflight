from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[3]
out=Path('/tmp/ch32h415-regression')
out.mkdir(parents=True, exist_ok=True)
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
host_include=out/'include'
host_include.mkdir(exist_ok=True)
(host_include/'platform.h').write_text('/* The host tests supply simulated peripherals. */\n')
run('pfic', '#include "platform/priority.h"\nstatic volatile uint32_t threshold;\n#define PFIC_ITHRESDR threshold\n'
    +fn(pf,'__attribute__((always_inline)) static inline uint32_t __get_BASEPRI')
    +fn(pf,'__attribute__((always_inline)) static inline void __set_BASEPRI_nb')
    +fn(pf,'__attribute__((always_inline)) static inline void __set_BASEPRI_MAX_nb')
    +(root/'src/test/ch32h415/pfic_test.c').read_text(),
    extra_flags=('-Wall','-Wextra','-I'+str(host_include),'-I'+str(root/'src/main'),'-I'+str(root/'src/platform/CH32/include')))

bb='src/platform/CH32/dshot_bitbang.c'
pre='''
#define USE_DMA_SPEC
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
 DEF_TIM(TIM1,CH1,NONE,0,8,0),DEF_TIM(TIM1,CH2,NONE,0,9,0),DEF_TIM(TIM1,CH3,NONE,0,10,0),DEF_TIM(TIM1,CH4,NONE,0,11,0),
 DEF_TIM(TIM8,CH1,NONE,0,8,0),DEF_TIM(TIM8,CH2,NONE,0,9,0),DEF_TIM(TIM8,CH3,NONE,0,10,0),DEF_TIM(TIM8,CH4,NONE,0,11,0)};
struct { const timerHardware_t *timhw; } bbPorts[4];
typedef struct {int owner;} resourceOwner_t;
static resourceOwner_t freeOwner={0},busyOwner={2};
static struct {struct {int useDshotBitbangedTimer;}dev;} config;
#define motorConfig() (&config)
static int lookupTimerIndex(int tim){return tim;}
static int timerGetNumberByIndex(int idx){return idx;}
static const timerHardware_t *timerGetAllocatedByNumberAndChannel(int t,int c){return NULL;}
static const resourceOwner_t *timerGetOwner(const timerHardware_t *t){return &freeOwner;}
static dmaoptValue_t dmaGetOptionByTimer(const timerHardware_t *t){return t->opt;}
typedef struct {dmaResource_t *ref;} dmaChannelSpec_t;
static int channel;
static const dmaChannelSpec_t *dmaGetChannelSpecByTimerValue(int t,int c,int o){static const dmaChannelSpec_t s={&channel}; return &s;}
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
#define CH32H4
#define USE_THEIA_OSD
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
static void *IOGetByTag(ioTag_t tag){(void)tag;return &ioToken;}
'''
run('timer_af_index',pre+fn('src/main/drivers/timer_common.c','const timerHardware_t *timerGetByTagAndIndex')+'''
int main(void){const timerHardware_t *first=timerGetByTagAndIndex(1,1);const timerHardware_t *second=timerGetByTagAndIndex(1,2);return first==NULL||first->tim!=TIM5||second==NULL||second->tim!=TIM9;}
''')

pre='''
#define FAST_IRQ_HANDLER
#define DMA_IT_TCIF 2
#define DMA_IT_HTIF 4
#define DMA_IT_TEIF 8
#define USART_FLAG_TC 1
#define USART_FLAG_RXNE 2
#define USART_FLAG_TXE 4
#define USART_FLAG_ORE 8
#define USART_FLAG_IDLE 16
#define ENABLE 1
#define DISABLE 0
#define SET 1
#define RESET 0
#define USART_IT_TXE USART_FLAG_TXE
typedef struct {uint32_t CFGR,CNTR,MADDR;} dma_t;
typedef struct {unsigned DATAR,STATR;} USART_TypeDef;
#define IS_DMA_ENABLED(d) (((dma_t *)(d))->CFGR & 1)
typedef struct {void *USARTx;dma_t *txDMAResource,*rxDMAResource;bool txDMAEmpty,txCompletePending;struct{uint8_t txBuffer[8];unsigned txBufferHead,txBufferTail,txBufferSize;uint8_t rxBuffer[8];unsigned rxBufferHead,rxBufferSize;void(*rxCallback)(uint32_t,void*);void*rxCallbackData;void(*idleCallback)(void);}port;} uartPort_t;
typedef struct {uartPort_t port;} uartDevice_t;
static unsigned usartFlags,monitorCalls;
static unsigned USART_GetFlagStatus(USART_TypeDef *u,int flag){(void)u;return usartFlags&flag;}
static void USART_ClearFlag(USART_TypeDef *u,int flag){(void)u;usartFlags&=~flag;}
static void USART_ITConfig(USART_TypeDef *u,int flag,int state){(void)u;(void)flag;(void)state;}
static void USART_SendData(USART_TypeDef *u,int data){(void)u;(void)data;}
static void uartTxMonitor(uartPort_t *p){(void)p;monitorCalls++;}
'''
run('uart_line_end_release',pre+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void uartTxMonitorIfComplete')+fn('src/platform/CH32/serial_uart_ch32bsp.c','FAST_IRQ_HANDLER void uartIrqHandler')+'''
int main(void){USART_TypeDef usart={0};uartDevice_t d={.port={.USARTx=&usart,.txDMAResource=NULL,.rxDMAResource=NULL,.port={.txBufferHead=2,.txBufferTail=1,.txBufferSize=8}}};
 usartFlags=USART_FLAG_TC;uartIrqHandler(&d.port);if(monitorCalls)return 1;
 d.port.port.txBufferTail=d.port.port.txBufferHead;usartFlags=USART_FLAG_TC;uartIrqHandler(&d.port);return monitorCalls!=1;}
''')

pre += '''
#define NVIC_PRIO_SERIALUART_TXDMA 0x50
#define ATOMIC_BLOCK(x) for(int once=1;once;once=0)
#define DMA_ARCH_TYPE dma_t
#define DMA_GET_FLAG_STATUS(d,f) ((d)->flags & (f))
#define DMA_CLEAR_FLAG(d,f) ((d)->flags &= ~(f))
typedef struct {uintptr_t userParam;uint32_t flags;} dmaChannelDescriptor_t;
static unsigned xDMA_GetCurrDataCounter(dma_t *d){return d->CNTR;}
static void xDMA_SetCurrDataCounter(dma_t *d,unsigned n){d->CNTR=n;}
static void xDMA_Cmd(dma_t *d,int enabled){d->CFGR=enabled;}
static void ch32DmaBarrier(void){}
'''
run('uart_usart_before_dma',pre+fn('src/platform/CH32/serial_uart_ch32bsp.c','void uartTryStartTxDMA(')+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void uartTxMonitorIfComplete')+fn('src/platform/CH32/serial_uart_ch32bsp.c','static void handleUsartTxDma(')+fn('src/platform/CH32/serial_uart_ch32bsp.c','void uartDmaIrqHandler(')+fn('src/platform/CH32/serial_uart_ch32bsp.c','FAST_IRQ_HANDLER void uartIrqHandler')+'''
int main(void){USART_TypeDef usart={0};dma_t reg={.CFGR=1,.CNTR=0};uartDevice_t d={.port={.USARTx=&usart,.txDMAResource=&reg,.rxDMAResource=NULL,.port={.txBufferHead=2,.txBufferTail=2,.txBufferSize=8}}};
 usartFlags=USART_FLAG_TC;uartIrqHandler(&d.port);if(monitorCalls||!d.port.txCompletePending)return 1;
 dmaChannelDescriptor_t desc={.userParam=(uintptr_t)&d,.flags=DMA_IT_TCIF};uartDmaIrqHandler(&desc);
 return reg.CFGR!=0||monitorCalls!=1||d.port.txCompletePending;}
''')
