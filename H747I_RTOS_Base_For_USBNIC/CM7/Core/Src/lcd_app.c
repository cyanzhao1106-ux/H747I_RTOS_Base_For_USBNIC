/**
  ******************************************************************************
  * @file    lcd_app.c
  * @brief   MIPI-DSI LCD bring-up + lightweight USB-NIC status dashboard.
  *
  *  The panel is brought up via the STM32H747I-DISCO BSP (DSI host + LTDC +
  *  DMA2D, framebuffer in external SDRAM at 0xD0000000). Once up, the screen
  *  shows a display-only dashboard (no touch) with a black background:
  *
  *    - the attached USB NIC's hardware info (chip, VID:PID, MAC, link state
  *      and speed, MTU, assigned IPv4);
  *    - the active CDC transport mode (ECM / NCM).
  *
  *  A dedicated low-priority FreeRTOS task owns all framebuffer writes; it polls
  *  the (cheap, lock-free) NIC snapshot, so the comparatively slow panel writes
  *  never stall the data path.
  ******************************************************************************
  */

#include "lcd_app.h"
#include "stm32h747i_discovery_lcd.h"
#include "stm32h747i_discovery_sdram.h"
#include "stm32_lcd.h"
#include "usbnic_log.h"
#include "usbnic_netif.h"
#include "usbh_rtknic.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* BSP glue                                                                  */
/* ------------------------------------------------------------------------- */

int32_t BSP_GetTick(void) { return (int32_t)HAL_GetTick(); }

/* Strong override of the BSP __weak hook: derive the LTDC pixel clock (PLL3)
 * from the same CSI source the validated system clock uses, instead of HSE.
 * M=1 -> 4 MHz (RCC_PLL3VCIRANGE_2), N=165 -> 660 MHz VCO (RCC_PLL3VCOWIDE),
 * R=24 -> 27.5 MHz pixel clock (identical to the stock HSE path 5*132/24). */
HAL_StatusTypeDef MX_LTDC_ClockConfig(LTDC_HandleTypeDef *hltdc)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  (void)hltdc;
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
  PeriphClkInitStruct.PLL3.PLL3M = 1U;
  PeriphClkInitStruct.PLL3.PLL3N = 165U;
  PeriphClkInitStruct.PLL3.PLL3P = 2U;
  PeriphClkInitStruct.PLL3.PLL3Q = 2U;
  PeriphClkInitStruct.PLL3.PLL3R = 24U;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 0U;
  return HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
}

/* ------------------------------------------------------------------------- */
/* Dashboard                                                                 */
/* ------------------------------------------------------------------------- */

#define GUI_REFRESH_MS   500U   /* ~2 Hz repaint of dynamic fields           */

#define MARGIN           16U
#define LABEL_X          MARGIN
#define VALUE_X          200U    /* values column (clears Font20 labels)     */

#define TITLE_Y          8U
#define SEP_Y            46U
#define ROW_Y0           64U     /* first info row                           */
#define ROW_PITCH        34U     /* info-row vertical pitch (Font20=20px)    */
#define ROW_FONT         Font20
#define TITLE_FONT       Font24

/* Info-row indices. */
enum {
  ROW_DEVICE = 0,
  ROW_VIDPID,
  ROW_MAC,
  ROW_LINK,
  ROW_MTU,
  ROW_IP,
  ROW_COUNT
};

static uint16_t        s_xsize, s_ysize;
static volatile uint8_t s_lcd_ready;     /* set once the panel is initialised */
static uint8_t          s_chrome_done;   /* static labels/title drawn once    */

#define COLOR_TITLE   UTIL_LCD_COLOR_CYAN
#define COLOR_LABEL   UTIL_LCD_COLOR_LIGHTGRAY
#define COLOR_VALUE   UTIL_LCD_COLOR_WHITE
#define COLOR_BG      UTIL_LCD_COLOR_BLACK

/* Map the Realtek product ID to a human-readable chip name. */
static const char *chip_name(uint16_t vid, uint16_t pid)
{
  if (vid != RTKNIC_VID_REALTEK) { return "Unknown"; }
  switch (pid)
  {
    case RTKNIC_PID_RTL8152: return "RTL8152B";
    case RTKNIC_PID_RTL8153: return "RTL8153";
    case RTKNIC_PID_RTL8156: return "RTL8156B";
    case RTKNIC_PID_RTL8157: return "RTL8157";
    case RTKNIC_PID_RTL8159: return "RTL8159";
    default:                 return "RTL815x";
  }
}

/* Compile-time CDC transport mode (matches the protocol module built in). */
#ifdef RTKNIC_USE_NCM
#define MODE_STR "Mode: CDC-NCM"
#else
#define MODE_STR "Mode: CDC-ECM"
#endif

static uint16_t row_y(uint32_t row) { return (uint16_t)(ROW_Y0 + row * ROW_PITCH); }

/* Clear a row's value area and draw fresh text there (LEFT_MODE honours Xpos). */
static void put_value(uint32_t row, const char *txt, uint32_t color)
{
  uint16_t y = row_y(row);
  UTIL_LCD_FillRect(VALUE_X, y, (uint32_t)s_xsize - VALUE_X, ROW_FONT.Height, COLOR_BG);
  UTIL_LCD_SetTextColor(color);
  UTIL_LCD_DisplayStringAt(VALUE_X, y, (uint8_t *)txt, LEFT_MODE);
}

/* Draw the fixed chrome once: title, mode, separator and row labels. */
static void draw_chrome(void)
{
  static const char *labels[ROW_COUNT] = {
    "Device:", "VID:PID:", "MAC:", "Link:", "MTU:", "IP:"
  };
  uint32_t i;

  UTIL_LCD_SetBackColor(COLOR_BG);

  UTIL_LCD_SetFont(&TITLE_FONT);
  UTIL_LCD_SetTextColor(COLOR_TITLE);
  UTIL_LCD_DisplayStringAt(MARGIN, TITLE_Y, (uint8_t *)"USB-NIC Monitor", LEFT_MODE);
  UTIL_LCD_DisplayStringAt(MARGIN, TITLE_Y, (uint8_t *)MODE_STR, RIGHT_MODE);

  UTIL_LCD_FillRect(MARGIN, SEP_Y, (uint32_t)s_xsize - 2U * MARGIN, 2U, COLOR_TITLE);

  UTIL_LCD_SetFont(&ROW_FONT);
  UTIL_LCD_SetTextColor(COLOR_LABEL);
  for (i = 0U; i < ROW_COUNT; i++)
  {
    UTIL_LCD_DisplayStringAt(LABEL_X, row_y(i), (uint8_t *)labels[i], LEFT_MODE);
  }
}

/* Render the live NIC fields. */
static void draw_nic(const usbnic_info_t *info)
{
  char buf[40];

  UTIL_LCD_SetFont(&ROW_FONT);

  if (info->present == 0U)
  {
    put_value(ROW_DEVICE, "-- not connected --", UTIL_LCD_COLOR_GRAY);
    put_value(ROW_VIDPID, "--",                  UTIL_LCD_COLOR_GRAY);
    put_value(ROW_MAC,    "--",                  UTIL_LCD_COLOR_GRAY);
    put_value(ROW_LINK,   "--",                  UTIL_LCD_COLOR_GRAY);
    put_value(ROW_MTU,    "--",                  UTIL_LCD_COLOR_GRAY);
    put_value(ROW_IP,     "--",                  UTIL_LCD_COLOR_GRAY);
    return;
  }

  put_value(ROW_DEVICE, chip_name(info->vid, info->pid), COLOR_VALUE);

  snprintf(buf, sizeof(buf), "0x%04X:0x%04X", info->vid, info->pid);
  put_value(ROW_VIDPID, buf, COLOR_VALUE);

  if (info->mac_valid)
  {
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             info->mac[0], info->mac[1], info->mac[2],
             info->mac[3], info->mac[4], info->mac[5]);
  }
  else
  {
    snprintf(buf, sizeof(buf), "unknown");
  }
  put_value(ROW_MAC, buf, COLOR_VALUE);

  if (info->link_up)
  {
    if (info->speed_dl == 0xFFFFFFFFU)
    {
      snprintf(buf, sizeof(buf), "UP  (>4 Gbps)");
    }
    else if (info->speed_dl >= 1000000U)
    {
      snprintf(buf, sizeof(buf), "UP  %lu Mbps",
               (unsigned long)(info->speed_dl / 1000000U));
    }
    else if (info->speed_dl > 0U)
    {
      snprintf(buf, sizeof(buf), "UP  %lu kbps",
               (unsigned long)(info->speed_dl / 1000U));
    }
    else
    {
      snprintf(buf, sizeof(buf), "UP");
    }
    put_value(ROW_LINK, buf, UTIL_LCD_COLOR_GREEN);
  }
  else
  {
    put_value(ROW_LINK, "DOWN", UTIL_LCD_COLOR_RED);
  }

  snprintf(buf, sizeof(buf), "%u", (unsigned)info->mtu);
  put_value(ROW_MTU, buf, COLOR_VALUE);

  if (info->ip4 != 0U)
  {
    snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
             (unsigned long)( info->ip4        & 0xFFU),
             (unsigned long)((info->ip4 >> 8)  & 0xFFU),
             (unsigned long)((info->ip4 >> 16) & 0xFFU),
             (unsigned long)((info->ip4 >> 24) & 0xFFU));
    put_value(ROW_IP, buf, COLOR_VALUE);
  }
  else
  {
    put_value(ROW_IP, "(no lease)", UTIL_LCD_COLOR_GRAY);
  }
}

static void LCD_Gui_Task(void *argument)
{
  (void)argument;

  for (;;)
  {
    if (s_lcd_ready != 0U)
    {
      usbnic_info_t info;

      if (s_chrome_done == 0U)
      {
        draw_chrome();
        s_chrome_done = 1U;
      }

      usbnic_get_info(&info);
      draw_nic(&info);
    }
    osDelay(GUI_REFRESH_MS);
  }
}

static const osThreadAttr_t lcdGui_attributes = {
  .name       = "lcdGui",
  .stack_size = 1024 * 4,
  .priority   = (osPriority_t) osPriorityLow,
};

void LCD_Gui_StartTask(void)
{
  (void)osThreadNew(LCD_Gui_Task, NULL, &lcdGui_attributes);
}

/* ------------------------------------------------------------------------- */
/* Panel bring-up                                                            */
/* ------------------------------------------------------------------------- */

void LCD_Demo_Init(void)
{
  uint32_t xsize = 0, ysize = 0;

  if (BSP_LCD_Init(0, LCD_ORIENTATION_LANDSCAPE) != BSP_ERROR_NONE)
  {
    LOG("LCD: BSP_LCD_Init FAILED");
    return;
  }
  BSP_LCD_GetXSize(0, &xsize);
  BSP_LCD_GetYSize(0, &ysize);
  BSP_LCD_DisplayOn(0);

  s_xsize = (uint16_t)xsize;
  s_ysize = (uint16_t)ysize;

  UTIL_LCD_SetFuncDriver(&LCD_Driver);
  UTIL_LCD_SetLayer(0);
  UTIL_LCD_Clear(COLOR_BG);

  s_lcd_ready = 1U;

  LOG("LCD: init OK (%lux%lu), status dashboard", (unsigned long)xsize,
      (unsigned long)ysize);
}
