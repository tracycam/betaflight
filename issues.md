# CH32H415 / 2026.6 迁移审计收敛清单

初审日期：2026-10-27。初审证据保留，当前状态以“修复后二次独立复核”为准；未烧写或进行硬件测试。

审计对象是当前 worktree 的实际源码和现有 ELF/BIN，不能把基线提交号当作已包含这些未提交改动的提交号。

- Worktree：`/home/rate/betaflight/.temp/ch32h415-2026.6`
- 分支：`ch32h415-2026.6-maintenance`
- 基线：`upstream/2026.6-maintenance`，`0db9244664892760da5823baa987faf7e1fac8bf`
- 参考仓库：`/home/rate/BF_H417`，HEAD `d37a7b647`。
- 配套实现：`/home/rate/H415_FC_Bootloader`、`/home/rate/TheiaOSD`。
- 硬件依据：Bootloader 仓库的 `docs/CH32H417RM.PDF`，V1.7；下文 PDF 页码指文件页码，章节号优先。
- 初审产物：`obj/main/betaflight_CH32H415.elf`；当时 BIN 为 518436 字节，SHA256 `989a8f30914a7eebe192ea7af6aadb47c1aba599c2ce36c108e73c5edce1d997`；修复后产物见下一节。

## 修复后二次独立复核（2026-10-27）

**结论：大部分原始修复有效，但不能接受下方全部“已修复”的关闭状态。仍有 5 项已复现的遗漏或回归，按严重度列在下面。** 此节针对 BIN `acfc1b6cc08960209aa6b5e0e80abe66a317d6804e09aa21983fe73babb48914`（515308 字节）及对应当前源码，优先于下一节修复者的完成声明。下文初审正文保留历史证据，不代表所有旧问题仍未修复。

### 二次复核修复结果

- R01：DMA TC 不再释放 TX 引脚；仅在 USART TC、FIFO 空且 DMA 停止时进入 monitor，启动新 DMA 前清旧 USART TC。
- R02：CH32 在配置阶段跳过 RX DMA 获取，不再先分配后置 NULL。
- R03：USB RX available/read 与 reset 使用同一 USB 优先级临界区，避免队列下溢。
- R04：Theia 保留 timer 不占动态 AF 序号，CLI 可继续枚举后续合法 AF。
- R05：DShot 启用 DMA TE 中断，错误时清 `telemetryPending` 并退出。
- 补充：SPI/DShot DMA 交接屏障已覆盖启停；`.noncacheable` 起始地址纳入 `_sbss.._ebss` 清零范围。
- 验证：CH32H415 构建、Theia producer、镜像段检查、完整 `checks`/`test-all` 及 STM32 `SPEEDYBEEF405WING` 回归均通过。最新 BIN 为 515360 字节，SHA256 `4ca36f84704028750f2e8c0002de3c37b0b184295f756765be06f726c461fbb4`。

### R01 / P1 — UART TX DMA 队列修好后，仍在最后一个字节发完前释放 TX 引脚（C06 未完全闭环）

- **位置**：`src/platform/CH32/serial_uart_ch32bsp.c:238`、`:255`；实际释放引脚在 `uartTxMonitor():179`。
- **触发**：UART 启用 TX DMA 和 `SERIAL_CHECK_TX`，DMA 最后一段已搬入 UART，但 USART TC 尚未置位，软件队列已空。当前 CH32 没有启用 `ENABLE_SERIAL_SKIP_CHECK_TX`，普通 UART MSP/GPS 打开端口时会设置此选项。
- **原因**：新增的 TC 处理中关 DMA EN 是正确的，随后 `uartTryStartTxDMA()` 可以把 `txDMAEmpty` 置 true；`handleUsartTxDma()` 立即调用 `uartTxMonitor()`，清 TE 并将 TX 改成输入上拉。DMA TC 只代表最后数据已经搬运，不代表最后停止位已经离开发送引脚。后面的 USART TC handler 无法挽回已经切走的引脚。
- **复现**：提取真实 handler、队列推进和 monitor 函数，输入 `DMA TC=1, UART TC=0, head=tail, TX_PIN_ACTIVE`，得到 `TE=0`、输入上拉、`TX_PIN_MONITOR`。ELF `uartDmaIrqHandler` 也包含此提前释放路径。原来的“下一段无法启动”用例已通过，但不能覆盖最后一段的线端完成。
- **建议**：DMA TC 只负责关 DMA、清标志及续传；仅当 USART TC 且没有新数据待发时释放引脚，区分 DMA 队列空与线端发送完成。增加最后一字节/停止位、空队列后续发和 TC 到达时又入队的验证。

### R02 / P2 — UART RX 回退 IRQ 后仍占着已分配的 DMA（C20 引入资源占用回归）

- **位置**：`src/platform/common/stm32/serial_uart_hw.c:112`；RX 分配实际发生在 `:313`。
- **触发 / 原因**：配置了有效 RX DMA option 时，`serialUART()` 先调用 `uartConfigureDma()`，完成 owner 登记、时钟和 mux 配置，然后才执行 `s->rxDMAResource = NULL`。清指针没有撤销分配，通道之后既不用于 RX，又不能再分给其他设备。
- **复现**：只设置 RX DMA、调用实际 `serialUART()` 后，`rxDMAResource=NULL` 但 owner 仍为 UART RX，其他用户的 `dmaAllocate()` 返回 false。
- **建议**：既然本轮选择 CH32 RX 统一走 IRQ，应在选择/申请 RX DMA 之前跳过该分支；不要先分配再丢指针。若采用释放方案，必须连同 mux、owner 和重新打开端口的状态一并处理。

### R03 / P2 — USB reset 与前台读取并发时可把空队列变成 2047 字节“可读”（C17 引入竞态）

- **位置**：`lib/main/CH32H41x/middlewares/board/cdc_vcp_ch32h41x.c:87`、`:229`。
- **触发 / 原因**：前台 `usb_vcp_get_rx_data()` 取得 count 后、更新 tail 前发生 USB RESET IRQ；新代码在 ISR 中把 head/tail 清零，前台恢复后仍按旧 count 读取并增加 tail。原先只有消费者修改 tail 的前提已被 reset 处理打破，读取端没有配套同步。
- **复现**：从 `head=2, tail=0` 开始，在长度快照后注入实际 RESET handler，再完成 1 字节读取，得到 `head=0, tail=1, available=2047`。空会话会向 MSP/CLI 暴露旧缓冲数据。
- **建议**：让读操作与 reset 对游标的修改互斥，或采用会话代次校验；保护范围须覆盖长度快照、复制和 tail 提交，仅在 reset 一侧包临界区不够。测试 RESET 插入读取各阶段及重新枚举后的第一帧。

### R04 / P2 — timer 枚举修复未改变返回结果（C18 仍未修复）

- **位置**：`src/main/drivers/timer_common.c:56`；CLI 消费者仍为 `src/main/cli/cli.c:7708`、`:7722`。
- **原因**：在 `index == timerIndex` 且当前项受保护时执行 `index++; continue;`，但固定的 `timerIndex` 没有改变，后续 index 只会更大。因此请求第 1 项时仍返回 NULL，CLI 仍把它当作枚举结束。
- **复现**：按真实表顺序使用 PA0 的 TIM2/TIM5/TIM9，CLI 遍历得到 0 个选项，而直接查询 index 2 可以得到 TIM5。
- **建议**：区分受保护项与枚举结束，或提供专用可用项迭代接口；保留已存 timer index 的语义，不能用无说明的重编号替代。补上“首项保留、后续可用”的调用链测试。

### R05 / P2 — DShot TE 错误分支可返回，但错误 IRQ 与状态收尾仍未接通（C23 部分修复）

- **位置**：`src/platform/CH32/dshot_bitbang_stdperiph.c:398`，以及 `:218`、`:312` 的重新配置；`src/platform/CH32/dshot_bitbang.c:288`。
- **原因**：所有启用中断处仍只有 `DMA_IT_TC`，没有 `DMA_IT_TE`。纯 TE 事件不会因这个通道进入新错误分支，状态也就不能及时报告。此外错误分支返回前没有清 `telemetryPending`，输入阶段出错会保留 pending，并继续使遥测等待走超时路径。
- **复现**：按实际中断配置注入纯 TE，得到 `IRQ delivered=0, status=OK, pending=1`；即使强制调用 handler，状态改为 DMA_ERROR 后 pending 仍为 1。ELF 的 `bbDMA_ITConfig` 参数也确认仅使能 TC。
- **建议**：在初始化和每次 DMA 重配后正确启用 TE 通知，或提供明确的错误轮询；统一清理 flags、pending、输入/输出状态，并定义后续帧恢复策略。覆盖输出错误和遥测输入错误。无限 ISR 循环已经移除，这一部分修复予以确认。

### 本轮通过的验证与保留项

- 原先 8 个主机最小复现全部通过：PFIC 嵌套比较、空电机 timer、pacer 耗尽、UART 分配拒绝、UART 连续 DMA、USB 主机停止读取、ADC 无 DMA、I2C 超时恢复。USB 用例现在有界返回，但观测到 **100 ms**，并非严格的单个 50 ms 总预算；内外两层等待预算仍可收紧。
- 额外 5 个测试使用实际源码函数、模拟寄存器/资源及指定中断交错，复现 R01–R05。它们证明控制流和状态问题，不是目标板波形测试。脚本及输出保存在 `/tmp/ch32h415-recheck/previous_probes.py`、`previous_results.txt`、`focused_probes.py`、`focused_results.txt`；本节记录了关键输入输出，避免结论只依赖临时文件。
- `make CH32H415 -j128` 返回成功，现有 HEX 为最新；本轮没有 clean rebuild。使用 WCH objcopy 从 ELF 重新生成临时 BIN，与报告的 BIN 逐字节相同；镜像段检查通过，长度和 SHA256 均与修复报告一致。
- 重新编译运行 Theia producer 测试（UBSan、`-Wall -Wextra -Werror`）通过。`git diff --check` 通过；本地工具链归档 SHA256 与新增固定摘要一致，下载规则已移除 `-k`。
- 静态检查确认 DShot DMAMUX/NULL 保护、解锁时拒绝字体请求、SPI/UART/USB 所列交接点的 fence、Flash LOCK/FLOCK 和 IRQ 入口状态恢复、USB PLL ready 等待、UART7/8 映射、GPIO speed、ADC MODE0、Theia 宏和显式 `.noncacheable` 段的修改已落地。
- **关闭状态需要收窄**：C06、C17、C20、C23 只能算部分修复，C18 仍开放；C15 仅修正了清标志地址，共享 STK_ISR 的双核 RMW 仍未改变。C01 的比较逻辑已修复，但零优先级屏蔽边界仍是明确保留的硬件验收项。C27 的 HS 分支不应列为整体已关闭。
- C09、C19、C33/C34、C36 保持原先保留状态。特别是 C09 的软件请求代码仍可能进入 WCH ROM；“目前使用硬件 BOOT0”是操作约束，不等于软件 ROM 路径已经禁用。
- 本轮没有重跑完整 checks、test-all、STM32 构建；它们属于修复者报告的验证结果。未烧写、未做硬件测试，也未改动固件源码。此次仅追加复核记录。

## 修复复核（2026-10-27，修复者记录）

以下状态针对当前未提交工作树，不改变后文原始审计记录。

- **已修复 P1**：C01、C02、C03、C04、C05、C06、C07、C08。PFIC 使用正确的零值及嵌套比较；SPI/UART/USB DMA 交接增加 `fence iorw, iorw`；电机 NULL 检查、DMAMUX 配置、UART 分配失败回退和 TX DMA 完成关断已补齐；USB 发送有界返回；解锁状态拒绝 Theia 字体写入。
- **已修复 P2**：C10、C11、C12、C13、C14、C15、C16、C17、C18、C20、C21、C22、C23。ADC 启动失败不再发布内部采样且 VREF 零样本有保护；I2C 允许本总线恢复；Flash 擦除传播状态/超时并恢复锁和中断入口状态；USB PLL、会话队列和速度相关状态已处理；UART7/8 映射补齐，RX DMA 明确回退 IRQ；周期使用实际核心频率和宽中间值；DMA 错误退出 ISR 并报告状态。
- **已修复 P3**：C24、C25、C26、C27、C28、C29、C30、C31、C32、C35。工具链启用 TLS 验证并固定 SHA256；Theia 宏、备用 UART 表、Full-Speed 上报、空读、bitbang 初始 GPIO、32 位 timer、GPIO 配置和 ADC MODE0 已修正；`.noncacheable` 已显式 NOLOAD。
- **保留/部分处理**：C09 需要应用与冻结 Bootloader 共同定义一次性软件 DFU 请求，当前仍只能依赖硬件 BOOT0，未伪造兼容；C19 需按具体 H415 型号及 VIO18 原理图确认；C27 仅修复当前 FS 上报，未来启用 HS 前仍须处理 512 字节 MPS 类型和分包；C33/C34 尚无足够证据直接改启动汇编或浮点语义；C36 属于未启用代码，暂不纳入本轮。
- **仍需硬件验收**：PFIC 对 0 优先级 IRQ 的实际屏蔽边界、DMA 长稳、连续 UART/USB、DShot 波形、Flash 掉电、Bootloader 跳转和 TheiaOSD 实际显示。软件修复及主机测试不等于台架通过。
- **修复后验证**：CH32H415 ELF/BIN/HEX 构建通过；Theia producer 测试及镜像段检查通过；完整 `checks`、`test-all` 和 STM32 `SPEEDYBEEF405WING` 回归见最终工作树记录。

## 结论与定级

部分认可第二份审查，但不认可其“已确认 4 项 P0”的整体结论。其中混有真实缺陷、已被调用链消除的风险、未启用分支的问题和需要实测的推断。尤其不能据当前证据断言 `.noncacheable` 导致 USB 核心随机初始化、DShot 解码位掩码错误、TIM9 CCR 地址错误或当前固件处于 High-Speed USB 模式。

本清单按以下标准排序：P1 为应优先处理的启动、实时性、数据传输或升级路径缺陷；P2 为特定配置、异常恢复或设备功能缺陷；P3 为潜伏分支、维护性和加固问题。未列已确认 P0，不等于认可当前固件可以试飞。源码和主机测试不能代替 PFIC、DMA、双核及电气行为的台架验收。

来源标记：“继承”表示 `BF_H417` 已有相同缺陷；“迁移回退”表示参考库已有处理而本次未保留；“本次新增”表示新实现或集成选择带来的问题。“条件性”表示需要指定配置或事件才触发，不代表已经在硬件复现。

## P1：优先修复

### C01 — PFIC 临界区嵌套会降低屏蔽强度

- **来源 / 重复**：继承；对应第二份审查 3.1，也是第一轮发现。
- **位置**：`src/platform/CH32/include/platform/platform.h:54`；原库实现位于 `src/main/build/atomic.h`。
- **证据**：`__set_BASEPRI_MAX_nb()` 以 `cur < priority` 决定更新。外层 `0x10`、内层 `0x50` 会变为 `0x50`；外层 `0x50`、内层 `0x10` 又不能收紧。提取实际函数的主机测试复现了两种错误。
- **影响**：嵌套的串口、时基等临界区不再满足“只能收紧屏蔽”的约定。
- **修复 / 验收**：使用 `priority != 0 && (cur == 0 || priority < cur)`，验证零值、相同值、双向嵌套及退出恢复，并检查 PFIC MMIO 与受保护访存之间的屏障。
- **相关待验证项**：`drivers/nvic.h` 将 SPI DMA、MPU EXTI、I2C IRQ 配为 `0x00`，而 `NVIC_PRIO_MAX` 是 `0x10`。不能假设这种映射在 PFIC 上具备 ARM BASEPRI 的全部语义；零值又表示阈值功能关闭。`spiSequence()` 在该临界区修改与 SPI DMA ISR 共享的链表，应按手册 §4.7.5.11 验证实际屏蔽关系及相等边界，必要时重排 CH32 优先级或使用保存/恢复全局中断状态的局部保护。比较函数修复本身不能证明这些 IRQ 已被屏蔽。当前 SysTick1 实际配置为 `0xF0`，不是第二份审查举例中的 `0x10`。

### C02 — V5F 的 DMA 缓冲区交接缺少硬件访存屏障

- **来源 / 重复**：继承；第二份审查 3.3 的有效补充。架构风险已确认，尚无硬件错包复现。
- **位置**：`src/platform/CH32/bus_spi_ch32h41x.c:240`、`:260`、`:289`；`src/platform/CH32/serial_uart_ch32bsp.c:220`；`lib/main/CH32H41x/middlewares/ch32h41x_hs/usb_ch32h41x_usbhs_reg.c:299` 及完成处理路径。
- **证据**：手册 §4.1.1 表 4-1/4-3 把 DTCM、SRAM 列为 Normal；允许 Normal store 与 Device store、Device load 与 Normal load 重排。当前路径直接从缓冲区准备进入 DMA/端点使能，完成后直接消费缓冲区；未建立完整的发布和获取屏障。`volatile` 和关闭编译器重排不能代替该硬件保证。
- **影响**：DMA 可能早于 CPU 写入可见时启动，或 CPU 早于 DMA 完成后的正确顺序读取数据。发生频率须实测。
- **修复 / 验收**：明确每个 CPU↔DMA 所有权交接点，使用覆盖内存和设备 I/O 的屏障，例如适用的 `fence iorw, iorw` 加编译器 `memory` clobber；验证 SDK 封装和最终指令。用长时间 SPI/UART/USB DMA 图样测试覆盖并发和完成回调。Theia 共享 RAM 的 `fence rw,rw` 不能自动证明外设 DMA 路径正确。

### C03 — 未配置或无有效 timer 的电机引脚被提前解引用

- **来源 / 重复**：继承；第一轮发现，第二份审查未覆盖。
- **位置**：`src/platform/CH32/dshot_bitbang.c:734`，尤其 `:739`。
- **触发 / 证据**：`timerGetConfiguredByTag()` 返回 NULL 后，代码先读取 `timerHardware->output`，再检查 IO。通用 target 没有板级电机引脚默认值；清空资源或缺少 timer 映射也可触发。实际函数的主机 UBSan 测试报告 NULL 成员访问。
- **影响**：初始化进入 C 未定义行为，不能保证安全返回“无可用电机”。是否在芯片上直接陷入异常取决于零地址映射，主机 UBSan 不等于目标板 HardFault 证明。
- **修复 / 验收**：在任何成员访问前验证 timer 和 IO，按维护版其他平台的失败路径返回；覆盖空配置、部分配置、受保护引脚和无 timer 映射。

### C04 — Bitbang DShot 保存了 DMAMUX 请求号，却未写入硬件

- **来源 / 重复**：继承；对应第二份审查 2.2，也是第一轮发现。
- **位置**：`src/platform/CH32/dshot_bitbang.c:263`、`:415`。
- **证据**：`bbPort->dmaChannel = dmaChannelSpec->dmaMuxId` 后，`bbSetupDma()` 只有 `dmaEnable()`、handler 和 timer DMA source 配置，没有 `dmaMuxEnable()`；后续 stdperiph 路径也没有补写请求选择。
- **影响边界**：选 TIM8 或其他与复位请求源不符的 pacer 时不能获得正确 DMA 请求；通道此前留下的 mux 配置也会影响结果。DMAMUX 复位选择可恰好对应 TIM1_CH1，因此不能断言所有默认配置必然完全无输出。
- **修复 / 验收**：分配成功后显式配置对应请求号；分别验证 TIM1/TIM8、多个 GPIO port、资源冲突后重选和双向遥测。

### C05 — UART DMA 分配被拒后仍配置别人的 DMA 通道

- **来源 / 重复**：**迁移回退**；第一轮发现，第二份审查未覆盖。
- **位置**：`src/platform/common/stm32/serial_uart_hw.c:293`、`:305`；下游 `src/platform/CH32/serial_uart_ch32bsp.c:90`。
- **证据**：分配失败没有清空 `txDMAResource` / `rxDMAResource`。下游仍按非 NULL 指针 DeInit/Init/Enable；外设地址可能仍为 0。主机拒绝分配测试复现该状态。`BF_H417` 对应函数有两个 `else { ...DMAResource = NULL; }`，本次遗漏。
- **影响**：合法的资源冲突会变成错误外设地址传输或破坏已有 DMA 用户，而不是退回 UART IRQ 模式。
- **修复 / 验收**：失败时清空相关资源状态、保留原 owner，并验证 RX/TX 分别退回 IRQ；包括与电机、SPI、其他串口冲突。

### C06 — UART TX DMA 完成后未关 EN，后续队列无法启动

- **来源 / 重复**：继承；第一轮发现，第二份审查未覆盖。
- **位置**：`src/platform/CH32/serial_uart_ch32bsp.c:195`、`:247`。
- **证据**：TC ISR 清标志后直接调用 `uartTryStartTxDMA()`；后者看到 EN 仍为 1 就返回。CH32 普通传输完成使计数归零，并不提供这里假设的自动清 EN 行为；参见手册 §10.2.1、§10.3.3。主机模拟 TC 后得到 `EN=1, CNTR=0, head=3, tail=1`，第二段没有启动。
- **影响**：启用 TX DMA 后，第一段之后的 UART 发送停住。
- **修复 / 验收**：按 CH32 的通道关闭、清标志和重新装载顺序处理完成；覆盖连续多段、环形缓冲区回绕、空队列后再发送、半双工收发切换。

### C07 — USB 发送缓冲满时无限等待，外层超时无效

- **来源 / 重复**：继承；对应第二份审查 8.1，也是第一轮发现。
- **位置**：`src/platform/CH32/serial_usb_vcp_ch32h4.c:107`、`:325`。
- **触发 / 证据**：主机停止读取 Bulk IN，设备仍处于 CONFIGURED，发送填满环形缓冲区。`CDC_Send_DATA()` 内部只循环 `delay(1)`，不能返回外层执行 `USB_TIMEOUT` 检查。主机测试在停止消费后超时未返回。
- **影响**：调用发送的协作调度任务可永久占住主循环；传输中拔线也没有内层退出保证。
- **修复 / 验收**：底层返回实际可排队字节数，或提供有界等待及连接状态检查；验证主机不读、拔插和缓冲区回绕时调度继续推进。

### C08 — Theia 字体写入在解锁状态仍可同步阻塞最长 1 秒

- **来源 / 重复**：**本次新增的同步适配行为**；第二份审查第三部分第 3 项的有效补充。
- **位置**：`src/main/io/displayport_theia.c:75`；调用入口 `src/main/msp/msp.c:4736`。
- **触发 / 证据**：MSP 字符写入进入 Theia `fontWrite()`，成功提交后等待完成，最长 1000000 μs；该路径没有 ARMED 检查。OSD 已就绪但随后不消费/不发布完成时，可达到超时上限。`BF_H417` 的字体作业采用异步完成轮询，本次为维护版同步 ACK 接口改成等待；任务文档也已记载这一选择。
- **影响**：飞行中收到该命令会阻塞主循环，不能以“正常字体通常很快完成”保证调度时限。
- **修复 / 验收**：至少在提交作业前拒绝解锁状态的字体写入；可在独立 Theia 驱动中检查，保持 ABI 隔离。随后评估有界异步接口。测试 ARMED 拒绝、不发布请求、超时及迟到完成。
- **纠正影响描述**：配套 `TheiaOSD/targets/h415_v3f/hal_font.c:22` 写 RAM 字库暂存区和提交标记，Flash 持久化由 Bootloader 处理；不能说该 MSP 帧会让 V3F 在飞行中直接擦写 Flash。

### C09 — 软件进入 Bootloader 的路径实际请求 WCH ROM ISP

- **来源 / 重复**：继承的 ROM 启动路径，与本次指定 Bootloader 的 DFU 目标不匹配；第二份审查第 4 项有效。
- **位置**：`src/platform/CH32/system_ch32h41x.c:40`、`:61`；`persistent_ch32h41x.c`；配套 `H415_FC_Bootloader/User/bl_boot.c:36`。
- **证据**：软件请求写 `RESET_BOOTLOADER_REQUEST_ROM`；应用再次启动后设置 Flash BOOT_MODE 并复位，代码明确目标 `0x1FFF0000`。配套 Bootloader 的 DFU 检查只看 PC8/BOOT0，不消费该软件请求。
- **影响**：CLI/Configurator 的软件升级请求不能按预期进入配套 USB DFU。硬件 BOOT0 进入 Bootloader 的通道是另一条路径，不受这一结论否定。没有证据表明请求本身会擦除 Bootloader 分区。
- **修复 / 验收**：先明确软件 DFU 是否属于冻结 Bootloader 契约。若需要，跨仓库约定可验证、一次性消费的请求和复位前屏障，由 Bootloader 在启动应用前处理；若当前契约不支持，应禁止/明确拒绝此软件路径并使用硬件 BOOT0。不能只在应用中改一个跳转地址就宣称兼容。

## P2：特定配置、恢复及外设功能

### C10 — DShot pacer 耗尽后继续遍历 NULL timer

- **来源 / 重复**：**迁移回退**；第二份审查 2.4，也是第一轮发现。
- **位置**：`src/platform/CH32/dshot_bitbang.c:322`、`:343`。
- **证据 / 影响**：某个 `bbPorts[]` 槽位找不到可分配 timer 后，外层继续迭代，在排重时调用 `timerGetTIMNumber(NULL)`。主机 UBSan 已复现。参考库在每轮搜索后有 `if (!bbPorts[bbPortIndex].timhw) break;`，本次未保留。
- **修复 / 验收**：恢复耗尽退出和失败状态；测试 TIM1/TIM8 被占、DMA 不足和多个电机 GPIO port。

### C11 — ADC 未成功启动仍被当作有有效内部采样，VREF 补偿可除零

- **来源 / 重复**：继承；第一轮发现，第二份审查未覆盖。
- **位置**：`src/platform/CH32/adc_ch32h41x.c:185`、`:287`、`:372`；`src/main/drivers/adc.c:59`。
- **证据 / 影响**：内部通道先标记 enabled，随后因 DMA option 未配置/不可用提前返回；busy 接口仍报告可读取，零初始化样本进入 VREF 除法。通用 target 未提供 `ADC1_DMA_OPT`，默认 `DMA_OPT_UNUSED`。主机测试复现“未启动转换、enabled=1、raw=0”和除零。RISC-V 的实际除法结果/编译器行为不能用主机异常直接替代，但无效电压温度数据是明确问题。
- **修复 / 验收**：成功启动后再发布 ready，失败时禁用内部采样；同时对零样本、未校准值做有效性处理。测试默认 DMA、分配拒绝、首次有效转换前和正常采样。

### C12 — I2C 超时恢复被自身引脚 owner 拒绝

- **来源 / 重复**：继承；第二份审查第 10 项，也是第一轮发现。
- **位置**：`src/platform/CH32/bus_i2c_ch32h41x.c:198`、`:486`。
- **证据 / 影响**：超时调用 `i2cInit()`，入口拒绝任何非空 owner，包括该 I2C 自己先前注册的 SCL/SDA；没有重新 unstick/复位，busy 状态可持续保留。主机测试得到 `unstick=0, busy=1`。
- **修复 / 验收**：首次分配和本总线恢复分开；允许匹配 owner/index 的重初始化，清理事务和错误状态。验证 SDA 被拉低后释放、超时后下一笔成功以及外部 owner 不被夺取。

### C13 — UART 半双工忽略开漏及上下拉选项

- **来源 / 重复**：继承；第二份审查 9.3 有效。
- **位置**：`src/platform/common/stm32/serial_uart_hw.c:137`，CH32 分支 `:156`。
- **证据 / 影响**：已算出的 `pushPull`、`pull` 被丢弃，一律 `IOCFG_AF_PP`。请求开漏或特定空闲电平的单线设备得不到对应电气配置，可能争用总线或无法通信。不能把所有 SmartAudio/Tramp/F.Port/SRXL2 配置一概视为相同电气需求。
- **修复 / 验收**：根据 CH32 GPIO 实际能力兑现选项，说明需要外部电阻的情形；按具体设备/BOM 测量空闲、换向和双方驱动时波形。

### C14 — Flash 配置写入的锁、错误状态和 IRQ 恢复不完整

- **来源 / 重复**：继承；第二份审查 11.1–11.3 有效，SDK Flash 文件与原库相同。
- **位置**：`src/platform/common/stm32/config_flash.c:382`、`:426`、`:452`；`lib/main/CH32H41x/Peripheral/src/ch32h417_flash.c:105`、`:718`、`:734`、`:777`。
- **证据 / 影响**：Unlock_Fast 解普通锁和快写锁，而 Lock_Fast 只置 FLOCK；配置清标志函数为空；ErasePage 的忙等待无超时且始终返回初始 COMPLETE；ProgramPage_Fast 无条件重新打开全局中断，没有恢复入口状态。后续编程的状态检查仍可能发现错误，因此“所有擦写错误都被静默吞掉”也不准确。
- **修复 / 验收**：成对锁回 LOCK/FLOCK；按手册清状态、传播擦除失败并设置超时；保存并恢复 WCH 的实际中断使能状态，而不是假设入口必开中断。测试写保护、忙超时和入口中断关闭。SDK `__disable_irq()` / `__enable_irq()` 操作 CSR `0x800`，不宜只按 ARM 或普通 `mstatus.MIE` 描述修复。
- **契约条件**：8 KB 页和镜像容量依赖 DBMODE。应在目标硬件验收中核对/拒绝不匹配布局，不应自动改 option bytes。配套 Theia 的运行 `.text/.rodata` 在 RAM_CODE，不能据应用入口在 Flash 推导“每次保存都挂起整个 V3F 视频 XIP”。

### C15 — SysTick 初始化清标志地址错误；共享状态 RMW 有条件竞态

- **来源 / 重复**：继承；第二份审查 5.1 部分有效。
- **位置**：`src/platform/CH32/system_ch32h41x.c:169`；`src/platform/common/stm32/system.c:119`；SDK `core_riscv.h:151`。
- **证据 / 影响**：初始化访问 `SysTick1->ISR`，但手册 §4.6.1 的状态寄存器在 `0xE000F004`，不是 `0xE000F084`；不能保证清除 V5F 旧挂起位。ISR 对共享状态做 RMW，如果 V3F 同时更新 bit 0，存在丢失更新的可能。
- **边界 / 修复**：正常配套 Theia 用 TIM12，不能断言它一直在与 BF 并发清 SysTick0；Bootloader fallback/延时路径另行核对。使用正确地址，并明确双核使用约束或同步方案。**单字节写不是解决方案**：bit 0、bit 1 都在同一个字节。不要在未核对写清零语义及 PPB 原子操作支持前套用 AMO 或 W1C。

### C16 — USBHS PLL 重新开启后没有等待 ready

- **来源 / 重复**：继承；第二份审查 8.3 有效，实际位置在 BF 初始化函数。
- **位置**：`src/platform/CH32/serial_usb_vcp_ch32h4.c:429`。
- **证据 / 影响**：重配并开启 USBHS PLL 后直接启 UTMI 和控制器，没有 ready 轮询。此前 PLL 状态和锁定速度可能掩盖问题，不能以一次枚举成功证明时序正确。
- **修复 / 验收**：等待 `RCC_FLAG_USBHSPLLRDY` 并有超时错误处理；验证冷启动、热复位和 Bootloader 不同进入路径。

### C17 — USB reset/reconfigure 未重置跨会话的软件队列状态

- **来源 / 重复**：继承；由第二份审查 8.2 收窄得到。
- **位置**：`lib/main/CH32H41x/middlewares/board/cdc_vcp_ch32h41x.c:85`；`src/platform/CH32/serial_usb_vcp_ch32h4.c:184`。
- **证据 / 影响**：CONFIGURED **会清** `ep_tx_busy_flag`，所以“拔插后永久 busy”并不成立。但 RX head/tail、TX in/out 和 `lastBuffsize` 没有成套重置；中途 reset 后，SOF 路径可能把被取消的旧传输当作完成推进，或把旧会话数据带到新会话。
- **修复 / 验收**：定义总线 reset/断连时丢弃或保留数据的策略，原子地清理端点和软件队列；测试 IN/OUT 中途 reset、未读 RX 数据和重枚举后的第一笔 MSP。

### C18 — 受保护 timer 形成枚举空洞，CLI 找不到后续合法 AF

- **来源 / 重复**：继承；第二份审查第 6 项没有指出此实际问题。
- **位置**：`src/main/drivers/timer_common.c:46`；`src/main/cli/cli.c:7708`、`:7722`；`src/platform/CH32/timer_ch32h41x.c:95`、`:133`。
- **证据 / 影响**：查询指定序号遇到 Theia 保留 timer 返回 NULL，CLI 却把第一个 NULL 当作全部选项结束。例如 PA0 的首项 TIM2 被拒绝，后面的 TIM5/TIM9 也无法通过该遍历列出/选择。原库已有相同过滤方式。
- **修复 / 验收**：区分“该项被保护”和“枚举结束”，或提供跳过保留项的迭代接口；注意已保存 index 的兼容性，不能随意重编号。验证 PA0/PA2/PA3 等存在多种 AF 的可用引脚。

### C19 — CH32H415 未执行手册要求的 VIO18 LDO 关闭配置

- **来源 / 重复**：继承遗漏；第二份审查第三部分第 1 项有效。
- **位置**：`src/platform/CH32/startup/system_ch32h417.c`、`src/platform/CH32/system_ch32h41x.c`；配套 Bootloader 初始化也未找到相应设置。
- **证据 / 影响**：手册 §2 的 PWR_CTLR 注释（PDF 第 12 页）要求未外封 VIO18、内部与 VDDIO 相连的型号在上电后通过软件选择 `VSEL_VIO18=111b` 关闭 LDO，降低功耗。当前仅有 SDK 定义，没有实际初始化调用。
- **修复 / 验收**：限定适用芯片，按手册设置 `VIO_SW_CR` 和 `VSEL_VIO18`；测电流及温升。确认的是遗漏和功耗风险，尚无证据量化发热或证明电源损伤。

### C20 — UART DMA 功能表与接收回调能力不完整

- **来源 / 重复**：继承；第二份审查 9.2、9.4 有效，但其默认 CRSF 影响描述过度。
- **位置**：`src/platform/CH32/dma_reqmap_mcu.c:59`、`:104`；`src/platform/CH32/serial_uart_ch32bsp.c:90`；`src/main/drivers/serial_uart.c:334`。
- **证据 / 影响**：UART7/8 已启用，却没有 DMA 请求映射，因此配置 DMA 时退回 IRQ。RX DMA 分支没有启用 IDLE，也不派发 IRQ RXNE 的逐字节 `rxCallback`。默认 UART RX DMA 为 UNUSED，CRSF/SRXL2 正常默认走 IRQ；若为依赖这些回调的协议强行选择 RX DMA，可能丢失接收处理，不能仅描述成“轮询慢一点”。
- **修复 / 验收**：补全请求映射；明确哪些接收协议支持 DMA，不支持时拒绝配置/退回 IRQ；支持时实现对应 IDLE 和数据投递。分别测试 UART7/8、CRSF/SRXL2 的实际配置，而非只打开宏。

### C21 — 周期换算的有符号中间乘法溢出，时钟比固定为 2

- **来源 / 重复**：继承；第二份审查 5.2、5.3 需要收窄。
- **位置**：`src/platform/common/stm32/system.c:224`、`:246`、`:256`。
- **证据 / 影响**：`100 * clockCycles` 的 int32_t 中间结果在 350 MHz、约 61.36 ms 的输入周期就溢出；十分之一微秒版本也有对应更大的边界。现有主要调用用于调度诊断，不能推导普通 125 μs 控制周期必然错误。固定 2 倍换算只适用于当前 350/175 MHz 配置。
- **修复 / 验收**：用足够宽的中间值、明确定义输入范围，从实际核心/计数器频率换算；覆盖边界和不同分频。SysTick 使用 HCLK 是合理的，CLI 主频读取 SystemCoreClock，第二份审查“CLI/MSP 报一半主频”的证据不足。

### C22 — DMA CFGR 清 EN 使用 16 位掩码，破坏高位状态

- **来源 / 重复**：继承 SDK/驱动缺陷；第二份审查 7.2 有效。
- **位置**：`lib/main/CH32H41x/Peripheral/src/ch32h417_dma.c:51`、`:205`；`src/platform/CH32/pwm_output_dshot.c:90`。
- **证据 / 影响**：32 位 CFGR 与 `(uint16_t)~DMA_CFGR1_EN` 相与，会清掉高 16 位，包括双缓冲 current-memory 位。现有普通/循环 DMA 路径不等于已经使用该双缓冲状态，不能据此断言当前 DShot 必坏。
- **修复 / 验收**：使用与寄存器宽度一致的掩码，检查 SDK 所有同类 RMW；验证保留高位和双缓冲切换。

### C23 — Bitbang DMA 错误没有可恢复或有界的退出路径

- **来源 / 重复**：继承；第二份审查 2.6 的有效部分。
- **位置**：`src/platform/CH32/dshot_bitbang.c:287`；`src/platform/CH32/dshot_bitbang_stdperiph.c:398`。
- **证据 / 影响**：handler 观察到 TEIF 后直接死循环。当前只启 TC interrupt，因此不能声称每个 TE 都必然立即进入该死循环；但这也没有建立传输错误后的正常收尾和故障报告路径。
- **修复 / 验收**：按 DMA 错误语义处理停通道、状态清理和输出失败，避免无限驻留 ISR；注入 TE/无完成事件，验证主循环仍可运行并报告故障。

## P3：低风险、潜伏分支与维护项

以下仍为待处理项，但不能拿来证明当前镜像必然发生第二份审查描述的飞行故障。

| ID | 问题与准确边界 | 来源及修复方向 |
|---|---|---|
| C24 | `.noncacheable` 是未显式管理的孤儿段，位于 `_edata` 与 `_sbss` 之间；初始化策略脆弱。但 USB 核心在使用前已经整体 memset。 | 继承相同链接脚本。显式定义段、NOLOAD/加载及初始化策略，并用 image check 覆盖。不是已确认的 USB 随机崩溃。 |
| C25 | `timer_def.h:28`、`timer_ch32h41x.c:37` 仍用 `USE_INTERNAL_OSD`，导致保留 timer 的定义和 handler 编入。 | 本次宏重命名遗漏。改为一致的 `USE_THEIA_OSD`；已有资源查询、DMA mapping 和分配保护仍有效，不能据向量表有 handler 就认定 CLI 可占用它。 |
| C26 | `serial_uart_ch32h41x.c:256` 的 UART6 TX 硬件表字段错误地填 RX 请求号。 | 继承。当前启用 `USE_DMA_SPEC`，实际使用 reqmap 中正确的 USART6_TX；修正备用表并覆盖两种构建分支，不应归因当前 UART6 DMA 失败于该未用字段。 |
| C27 | USB 控制器配置为 Full-Speed，而 `usbd_get_port_speed():161` 固定返回 HIGH；若开启 HS，局部 `mps:183` 和 `usbd_ep_info.mps:77` 的 uint8_t 都容不下 512。 | 继承。速度上报与实际模式一致，HS 启用前同时修正类型、描述符、分包/ZLP；当前 MPS=64 不能按 HS 违规处理。 |
| C28 | `NVIC_BUILD_PRIORITY(6,0)` 被掩码成 `0x80`，表达方式超出声明的两位抢占字段；`usbVcpRead():301` 空读永久等待。 | 继承。改用明确的 USB 优先级；空读接口应明确非阻塞契约。常规调用先检查可读数据，尚无当前空读死锁触发链，不能与 C07 等量定级。 |
| C29 | Bitbang 初始化/enable 使用 AF_PP，但实际 DMA 帧前 `bbSwitchToOutput()` 会改为普通 PP；GPIO 切换每次重算 16 个 pin。 | 继承。初始化也应使用适合 bitbang 的模式，测试启动/enable 空闲窗口；预计算 mask 的收益需实测，不应以循环存在便断言 8 kHz 不可用。 |
| C30 | TIM9..12 的 `timerSetPeriod()/timerGetPeriod()/timerSetCounter()` 直接使用 16 位字段，会截断超过 65535 的值；这与 `timerCCR()` 地址无关。 | 继承。位于 `timer_ch32bsp.c:953`。明确这些 timer 支持的范围或按型号使用 32 位字段；常见小周期 PWM 不因此自动失效。 |
| C31 | `IOConfigGPIO():105` 忽略请求速度，固定 Very_High；无效 input mode=3 时 GPIO_Mode 未赋值；AF 在启输出后才设置。 | 继承同一函数。兑现速度、初始化/拒绝非法配置，合理排序 AF 配置。输出 mode=3 本身有 AF_OD 分支，不应误说所有 mode=3 均未初始化；毛刺须示波器确认。 |
| C32 | SDK `ADC_SMP_ModeConfig():1230` 两分支都比较 MODE1，MODE0 分支不可达。出厂校准为空的处理也不健壮。 | 继承。MODE0 条件应修正；该 API 目前没有活跃调用。对校准空值的设备影响须实测，优先处理已证实的 C11。 |
| C33 | 启动汇编在 mtvec 前写入含 MIE 的 mstatus；Cache 操作应按语义核对；C 初始化再次复制已经执行中的相同 TCM 代码。 | 继承，汇编与原库相同。先建立向量/清挂起再允许中断，避免重复复制；当前汇编已有两条 `fence.i`，不能说整条启动复制链完全没有指令同步，也不能把相同内容重拷贝直接当作已复现代码损坏。 |
| C34 | `CH32H4.mk:105` 使用 `-fsingle-precision-constant`，改变无后缀浮点常量语义。 | 继承。建议与维护版统一或证明差异必要；先做实际数值回归。尚未找到证据证明当前 GPS 路径因此出现米级漂移，不能认定为已证实导航 P1。 |
| C35 | 新工具链下载规则 `src/platform/CH32/mk/tools.mk:11,40` 使用第三方二进制、`curl -k`，没有固定 SHA256。 | 本次新增的 Linux 工具链下载入口；旧构建系统其他下载也有类似写法，但不能称该新 URL 原库已有。启用 TLS 验证、固定内容摘要并记录来源，防止不可复现或替换下载。 |
| C36 | 可选 camera-control 的软件 PWM 分支含错误寄存器名、重复 TIM6/7 handler；硬件 PWM 接口也需适配。可选 USB MSC 分支含优先级/容量前置条件问题。 | 大部分继承的未启用代码，camera 文件与原库相同，接口迁移还需补齐。当前 target 未启这些功能；启用前分别编译并测试，不能计为当前启动阻断。 |

`-fsingle-precision-constant` 的语义可参见 [GCC Optimize Options](https://gcc.gnu.org/onlinedocs/gcc-15.1.0/gcc/Optimize-Options.html#index-fsingle-precision-constant)。这份文档只支持常量类型变化，不支持“本项目已经产生米级 GPS 漂移”的推断。

## 不采纳的结论与仍需验证的假设

| 第二份审查条目 | 收敛结论及反证 |
|---|---|
| 1：USB 核心未清零、热复位 intf_offset 每次加 2 | 不采纳。`cdc_acm_init():170` 先调用 `usbd_desc_register()`，后者 `usbd_core.c:1455` 清整个结构并置零 offset，再注册 interface。当前只有一次有效 add_interface 调用。ELF 中 `0x2014a95c` 调用注册函数，`0x2014a722` 调用 memset，长度为 `0x3fc`。保留的只是 C24。 |
| 2.1：USE_DSHOT_BITBAND 是 BITBANG 拼写错误 | 不采纳该因果关系。BITBAND 是另一种解码优化宏，`dshot_bitbang_decode.c/.h` 对 CH32 排除该优化；BITBANG 则来自通用 feature 配置。无效/多余宏可清理，但不是造成默认 bitbang 开启的拼写修复。 |
| 2.3：decode_bb 需要位掩码却传入 pinIndex | 不采纳。实际实现 `src/main/drivers/dshot_bitbang_decode.c:265` 接收 bit 序号，内部执行 `mask = 1 << bit`；pinIndex 正确。不能只依赖头文件形参命名推断接口。 |
| 2.5：重新放开 requestTelemetry=false，回退 a1871b58a | 不采纳。`dshot_bitbang.c:578` 附近的对应代码仍被注释，参考库修复保留。通用 `prepareDshotPacket()` 在编码后清一次性请求标志是另一处逻辑。 |
| 3.2：PFIC 0 优先级逃逸已经造成具体崩溃 | 保留 C01 的必须验收项，不能声称已经做过目标板中断注入。需要确认分组、阈值边界及共享数据的保护方式。修正 MAX 比较方向也不能替代这一步。 |
| 5.1：用 byte 写共享 STK_ISR 即可避免双核竞态 | 不采纳该修复。两个核的标志同在低字节；byte 写仍会影响对方位。正常 Theia 也并非用 SysTick0 提供视频节拍。 |
| 5.2：HCLK 当 CPU 主频导致 CLI/MSP 一律少一半 | 不采纳。当前 SysTick 按 HCLK 工作，mcycle 换算额外除以 2，CLI 直接用 SystemCoreClock。保留 C21 的固定比例及整数溢出问题。 |
| 6.1：旧 OSD 宏使 CLI 可以直接占用 TIM2/10/11/12 | 证据不成立。`timer_common.c:57` 拒绝这些 timer；`dma_reqmap_mcu.c:223,261` 拒绝相关请求/通道，`drivers/dma.c:31` 保留 DMA2_CH6。handler 被链接不等于 IRQ 被使能或资源被占用。保留 C25，另有实际枚举空洞 C18。 |
| 6.2：TIM9 CCR_32 位于不同地址，timerCCR 返回 16 位指针 | 不采纳。SDK `ch32h417.h:643` 把 CH1CVR/CH1CVR_32 定义在同一 union；`timCCR_t` 是 uint32_t，`timerCCR():904` 返回该 32 位指针。其他 period/counter 的宽度问题见 C30。 |
| 7.1：TRC 必然让多个通道都捕获 TI1，因而双向 PWM DShot 必坏 | 不能据 STM32 名称直接推导。CH32 手册 §14.3.11、§15.3.9 明确要求通道 2 双边沿捕获设置 CC2S=11b 和 AUX 对应位。共享 ARR/CNT 的切换时序仍值得台架测试，但需证明事件交错导致错误，不能仅凭这些寄存器赋值判定。 |
| 8.2：ep_tx_busy_flag 断连后永久不清 | 不采纳永久 busy 结论，CONFIGURED 分支明确清零。跨会话其他状态未重置见 C17。 |
| 8.4/8.5：当前 HS Bulk 64 非法，8 kHz SOF 全是无用中断 | 不采纳。当前 `CONFIG_USB_HS` 未定义，硬件 BASE_MODE 为 Full-Speed；SOF 用于 `usb_int_rxsof_handler()` 推进 TX，不是无用中断。当前是 FS 帧节拍，不能套用 HS 8 kHz。保留 C27 的速度上报与潜伏截断。 |
| 9.1：UART6 当前 DMA TX 因硬件表 RX 号必坏 | 该字段错误但当前路径不用它。启用 USE_DMA_SPEC 后查到的是 USART6_TX；实际 UART TX 首段后停顿见 C06。 |
| 11.4：配置 Flash 擦写必然让正常 Theia 视频因 XIP 停顿 | 不采纳。配套 `TheiaOSD/targets/h415_v3f/Ld/Link.ld:37` 把全部运行 `.text/.rodata` 放 RAM_CODE，Flash 入口只是启动 trampoline。启动重叠/取 Flash 资源的特殊路径需另外证明。 |
| 第三部分 3：字体命令直接让 V3F 飞行中写 Flash | 不采纳该附加影响；RAM 暂存协议和 Bootloader 提交见 C08。同步等待影响仍成立。 |
| 第三部分 4：unusedPinsInit 把调试脚变 IPU 导致调试断开 | 当前更直接的证据是 `usbVcpInit():422` 主动关闭 SWJ，并将 PB8/PB9 注册为 OWNER_USB。属于 USB/调试共享引脚的配置选择，不能按“没有 owner 被 unusedPins 抢走”修复。若需要运行中调试，应单独定义调试构建/硬件入口。 |
| 第三部分 8：LSM6DSK320X 与 IIM-42652 支持未迁移 | 不采纳这两项缺失结论。`common_pre.h:146,155` 已启用对应驱动，维护版已有识别；现有 ELF 也包含 LSM6DSK320X 相关函数。不能只检查 target.h。 |

确实存在但需要 BOM 决定的功能差异：参考库对 ICM-40608 的额外识别/量程适配和 `USE_AFOD_LED_STRIP` 外部上拉驱动模式未完整迁入。任务文档明确排除未经确认的板级外设扩展，因此列为条件性功能缺口；确认实际装配这些器件后再决定优先级。没有据此推断当前板上 IMU/灯带必坏。

## 两轮审查的对应关系

第一轮共有 9 项，其中 **5 项与第二份重复，4 项未被第二份覆盖**；第一轮问题中 **7 项继承自参考库，2 项是迁移回退**。

| 第一轮发现 | 本清单 | 第二份对应位置 | 来源 |
|---|---|---|---|
| 未配置电机 timer 的 NULL 解引用 | C03 | 未覆盖 | 继承 |
| Bitbang DShot 漏配 DMAMUX | C04 | 2.2 | 继承 |
| UART TX DMA TC 后 EN 未清 | C06 | 未覆盖 | 继承 |
| PFIC MAX 阈值比较反向 | C01 | 3.1 | 继承 |
| UART DMA 分配失败未清资源指针 | C05 | 未覆盖 | 迁移回退 |
| USB 满缓冲无界等待 | C07 | 8.1 | 继承 |
| DShot pacer 耗尽后 NULL 解引用 | C10 | 2.4 | 迁移回退 |
| ADC 无 DMA 仍消费内部采样并除零 | C11 | 未覆盖 | 继承 |
| I2C 恢复被自身 owner 拒绝 | C12 | 10 | 继承 |

新核实的有效补充包括：DMA 访存顺序、软件 DFU 目标不匹配、字体同步等待、半双工 GPIO 选项、Flash API 收尾、SysTick 清标志地址、USB PLL 和会话状态、VIO18 配置。它们大多是原驱动/SDK 已有问题；字体同步等待是本次适配行为。旧 OSD 宏未随重命名更新、新工具链下载入口则属于本次集成问题。不能把第二份审查所有条目都归为本次移植回退。

## 验证证据与边界

- 第一轮从实际源码提取函数、模拟外设/资源状态并使用主机 UBSan，复现 C01、C03、C05、C06、C07、C10、C11、C12；结果是软件控制流证据，不是硬件飞行记录。审计现场脚本和日志位于 `/tmp/ch32h415-audit/probe.py`、`results.txt`，本文件已记录关键输入与输出，避免仅依赖临时文件。
- Theia producer ring 及配套真实 consumer/font staging 的主机测试通过，未发现这轮待核对指控所称的 ring ABI 隔离破坏。
- 第一轮执行 `make CH32H415 -j128` 成功，但已有目标为最新，**没有做全量 clean rebuild**。用户记录的 checks、test-all、STM32 回归不应冒充本轮重新执行的检查。
- 现有 ELF/BIN 镜像检查通过，入口 `0x08016000`；本轮再次核对 ELF 段表、USB 注册调用反汇编和 BIN 摘要。镜像检查通过只证明受检查的布局约束，不证明驱动时序正确。
- 来源判断结合函数逐项对比；SDK DMA/Flash/ADC、启动汇编、持久化实现等多个文件与 `BF_H417` 相同。UART 分配拒绝回退和 pacer 退出保护的缺失则有原库明确代码反证。
- 未执行硬件上电、烧写、PFIC 中断注入、示波器测量、USB 枚举/吞吐或 DShot 电调测试。P1 修复后应围绕对应触发条件做台架验收，再决定是否满足试飞条件。

本次工作只新增审计清单，没有修改固件实现、Bootloader 契约或生成新的固件产物。
