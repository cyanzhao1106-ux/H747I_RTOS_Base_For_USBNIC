/**
  ******************************************************************************
  * @file    lcd_app.h
  * @brief   MIPI-DSI LCD bring-up + lightweight status dashboard (public API).
  ******************************************************************************
  */

#ifndef __LCD_APP_H
#define __LCD_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

/* Bring up the DSI/LTDC panel and clear it to black. Call before the scheduler
 * starts, while HAL_Delay (TIM6) is live. */
void LCD_Demo_Init(void);

/* Create the low-priority FreeRTOS task that renders the status dashboard
 * (NIC info + transport mode + live CPU usage). Call after
 * osKernelInitialize() and before osKernelStart(). */
void LCD_Gui_StartTask(void);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_APP_H */
