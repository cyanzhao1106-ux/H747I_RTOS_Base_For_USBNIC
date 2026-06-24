# CLAUDE.md — H747I_RTOS_Base_For_USBNIC

本文件为项目级指引，供 Claude Code 在本仓库工作时遵循。请在此基础上补充。

## 项目目标 (Goal)

在 STM32H747I-DISCO 的现有 RTOS 工程中，集成 Realtek USB LAN 芯片驱动 + lwIP 协议栈，
实现 USB 网卡（USB NIC）功能。

- 支持芯片：**8152B / 8153B**（2 个 USB 配置：Config1=Vendor，Config2=CDC-ECM）
  与 **8156B / 8157 / 8159**（3 个配置：Config1=Vendor，Config2=CDC-NCM，Config3=CDC-ECM）。
- 分支策略：共享基线 `feature/usbnic-base`，再 fork 出
  - `feature/usbnic-ecm`（ECM，全部 5 款芯片）
  - `feature/usbnic-ncm`（NCM，仅 8156B/8157/8159）
- 协议栈：**lwIP 2.2.1**，启用 DHCP client / ICMP / UDP / TCP；API 面 = sockets + netconn + raw。
- 首个验证里程碑：DHCP + ICMP ping 响应。

## 硬件 / 平台 (Hardware)

- MCU：STM32H747XI 双核（Cortex-M7 + Cortex-M4）。**USB NIC 相关代码运行在 CM7**。
- USB：OTG_HS，经外部 **ULPI PHY** 高速运行（外部 VBUS，DMA 关闭）。
- RTOS：CMSIS-RTOS v2 over FreeRTOS Kernel V10.6.2；HEAP_4；configTICK_RATE_HZ=1000。
- USB Host：ST STM32_USB_Host_Library；`USBH_USE_OS=1`。

## 构建 (Build)

**CubeMX 未安装**——`.ioc` 仅供参考，所有工程变更需手工编辑 Keil `.uvprojx`。

Keil MDK 无界面编译（UV4 为 GUI 程序，PowerShell 需 `Start-Process -Wait`）：

```powershell
$proj = "<abs>\MDK-ARM\H747I_RTOS_Base_For_USBNIC.uvprojx"
$log  = "<abs>\MDK-ARM\build_log.txt"
Start-Process -Wait -PassThru -FilePath "C:\Keil_MDK-ARM\UV4\UV4.exe" `
  -ArgumentList @('-b', $proj, '-t', 'H747I_RTOS_Base_For_USBNIC_CM7', '-j0', '-o', $log)
Get-Content $log
```

- CM7 目标名：`H747I_RTOS_Base_For_USBNIC_CM7`。
- 编译日志在 `MDK-ARM/build_log.txt`。

## 目录结构 (Layout)

- `CM7/USBNIC/`  — Realtek USB NIC 类驱动（`usbh_rtknic.c/.h`、`usbh_rtknic_ecm.c`、NCM 分支 `usbh_rtknic_ncm.c`）。
- `CM7/LWIP/`    — lwIP 接入层（`usbnic_netif.c/.h`、`lwipopts.h`）。
- `CM7/USB_HOST/` — ST USB Host 应用层与配置（`usbh_conf.h` 等）。
- `CM7/Core/`    — `main.c`、`FreeRTOSConfig.h`。
- `Middlewares/ST/STM32_USB_Host_Library/` — ST 主机库（含少量 additive 修改）。
- `Middlewares/Third_Party/LwIP/`          — vendored lwIP 2.2.1。
- `MDK-ARM/`     — Keil 工程、startup 文件、链接脚本。

## 关键设计要点 (Key design notes)

- **配置切换**：ST 枚举器只取 config index 0（Realtek vendor config，接口类 0xFF）。
  自定义类以 ClassCode 0xFF 被选中后，扫描各 config 并发 SET_CONFIGURATION 切到 ECM/NCM。
- **数据接口查找**：`USBH_FindInterface` 会返回 alt0（0 端点）的错误 slot；
  驱动手工遍历 `CfgDesc_Raw` 找到带 bulk 端点的 data alt setting（`RTKNIC_ScanConfig`）。
- **additive 主机库改动**：`usbh_ctlreq.c/.h` 新增 `USBH_Get_CfgDesc_Idx()`，按索引取非默认 config 描述符。
- **ECM 收发**：每次 bulk transfer 一个裸以太帧（无 FCS），以 short/ZLP 结束。
- **NCM 收发**（仅 NCM 分支）：NTB 帧封装多路 datagram。
- **线程安全**：USB host task 与 lwIP TCP/IP thread 之间——控制面经 `tcpip_callback()`，
  RX 经 `tcpip_input()`，单帧 TX 交接由 `volatile tx_busy` 保护。
- **堆划分**：FreeRTOS heap（任务栈/OS 对象，64KB）；newlib heap（USBH handle malloc，
  startup `Heap_Size=0x4000`）；lwIP 自有静态 mem/pbuf 池（.bss）。
- 编译期协议选择：NCM 分支在 CM7 目标预处理宏中加 `RTKNIC_USE_NCM`。

## 约定 (Conventions)

- 新增源文件后，必须同步在 `.uvprojx` 的 CM7 目标里登记 `<Group>/<File>` 并更新 `<IncludePath>`。
- 改动 ST 主机库时保持 additive（只增不改语义），便于将来同步上游。
- 提交信息结尾保留 Co-Authored-By 行（见全局规范）。

## lwIP 移植踩坑记录 (lwIP port gotchas — armclang / Arm Compiler 6)

集成 lwIP 2.2.1 时遇到并已修复的编译/链接问题，供后续参考：

1. **`arch/bpstruct.h' file not found`**
   `src/system/arch/cc.h` 同时定义了 `__attribute__((packed))` 打包 **和** `PACK_STRUCT_USE_INCLUDES`。
   二者互斥——后者要求 include 式打包头 `bpstruct.h/epstruct.h`。
   修复：删除 `PACK_STRUCT_USE_INCLUDES`，只保留 attribute 打包。

2. **`use of undeclared identifier 'configTICK_RATE_HZ' / 'configMINIMAL_STACK_SIZE'`**（sys_arch.c）
   OS 抽象层不应依赖 `FreeRTOSConfig.h`。改用运行时 CMSIS API：
   `osKernelGetTickFreq()` 求 tick 率；线程栈回退用固定字节值。

3. **`Undefined symbol errno` / 之后 `ENOBUFS、EWOULDBLOCK ... undeclared`**
   Arm C 库 `<errno.h>` 只有 EDOM/ERANGE 等少量值，缺 BSD socket errno 集。
   修复：`cc.h` 用 `#define LWIP_PROVIDE_ERRNO 1`（由 lwIP 提供全套 E* 宏），
   并在 `sys_arch.c` 提供唯一的 `int errno;` 存储。

4. **`L6406E: No space in execution regions`**（链接期）
   CM7 scatter `stm32h747xx_flash_CM7.sct` 原本只用 DTCM（`0x20000000`，128K），
   放不下 lwIP 静态池 + 64K FreeRTOS heap + newlib heap。
   修复：新增 `RW_IRAM2 0x24000000 0x00080000`（AXI-SRAM 512K）。OTG_HS DMA 已关，放此处安全。

> 当前 CM7 基线编译结果：0 Error / 0 Warning，Code≈114K，ZI≈160K。

## NCM 分支说明 (feature/usbnic-ncm)

`usbh_rtknic_ncm.c` 实现 CDC-NCM 协议（仅 8156B/8157/8159），通过 `RTKNIC_NCM_Ops` 接入核心驱动。
- 仅在 CM7 target 预处理宏中加 `RTKNIC_USE_NCM` 时启用（`RTKNIC_ACTIVE_OPS` 选 NCM）。
- 帧格式：NTB-16（NTH16 12B + NDP16 + datagrams）。RX 遍历所有 NDP16 投递各 datagram；
  TX 把单个以太帧封进最小单 datagram NTB-16（固定 4B 对齐布局：NTH@0 / NDP@12 / datagram@28）。
- bring-up：GET_NTB_PARAMETERS → SET_NTB_INPUT_SIZE(≤RX_BUF 2048) → SET_ETH_PACKET_FILTER
  → 读 iMACAddress → fallback MAC。每步容错。
- `h->priv[0]`=TX 序号，`h->priv[1]`=设备 dwNtbInMaxSize。
- ECM 源文件仍参与编译（`RTKNIC_ECM_Ops` 定义但不被引用），由 `RTKNIC_USE_NCM` 在编译期决定实际协议。

<!-- 在此补充你的基础知识 / Add your own notes below -->
