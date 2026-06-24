/**
  ******************************************************************************
  * @file    usbnic_log.h
  * @brief   Lightweight UART debug log over the ST-LINK-V3 VCP (USART1).
  *
  *  H747I-DISCO: the on-board ST-LINK-V3E exposes a USB Virtual COM Port that
  *  is wired to STM32 USART1  ->  PA9 (TX) / PA10 (RX), AF7.
  *  Open the COM port at 115200 8N1 on the PC side to read these logs.
  ******************************************************************************
  */

#ifndef __USBNIC_LOG_H
#define __USBNIC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"
#include <stdio.h>

/* Bring up USART1 (115200 8N1) on the VCP pins. Safe to call before the RTOS
   kernel is started; uses HAL_GetTick() for timestamps. */
void LOG_Init(void);

/* Format one whole line (timestamp prefix + CRLF) into a local buffer and push
   it out with a single HAL_UART_Transmit. Doing the UART work per-line instead
   of per-character (the old printf->fputc path) collapses the number of points
   at which the calling task can be preempted mid-line from dozens to one, so
   runtime logs stream out smoothly instead of dribbling byte-by-byte. */
void LOG_Printf(const char *fmt, ...);

/* Printf-style log line, prefixed with a millisecond timestamp and CRLF-terminated. */
#define LOG(fmt, ...)   LOG_Printf(fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __USBNIC_LOG_H */
