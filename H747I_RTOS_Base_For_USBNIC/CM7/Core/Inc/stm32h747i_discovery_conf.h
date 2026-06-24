/**
  ******************************************************************************
  * @file    stm32h747i_discovery_conf.h
  * @author  MCD Application Team
  * @brief   STM32H747I_DISCO board configuration file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2018 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef STM32H747I_DISCO_CONF_H
#define STM32H747I_DISCO_CONF_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* COM define */
#define USE_COM_LOG                         0U
#define USE_BSP_COM_FEATURE                 0U

/* LCD controllers defines.
 * The MB1166 daughterboard ships with either an OTM8009A or an NT35510 panel
 * driver IC; enabling both lets BSP_LCD_Init() auto-probe and bind the right
 * one. ADV7533 (HDMI bridge) is disabled — it pulls in the I2C4 bus driver,
 * which we deliberately leave out of this minimal LCD bring-up. */
#define USE_LCD_CTRL_NT35510                1U
#define USE_LCD_CTRL_OTM8009A               1U
#define USE_LCD_CTRL_ADV7533                0U

#define LCD_LAYER_0_ADDRESS                 0xD0000000U
#define LCD_LAYER_1_ADDRESS                 0xD0200000U

#define USE_DMA2D_TO_FILL_RGB_RECT          0U

/* IRQ priorities */
#define BSP_SDRAM_IT_PRIORITY               15U
#define BSP_BUTTON_WAKEUP_IT_PRIORITY       15U

#ifdef __cplusplus
}
#endif

#endif /* STM32H747I_DISCO_CONF_H */
