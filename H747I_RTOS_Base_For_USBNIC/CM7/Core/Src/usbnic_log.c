/**
  ******************************************************************************
  * @file    usbnic_log.c
  * @brief   UART debug log over the ST-LINK-V3 VCP (USART1, PA9/PA10, AF7).
  *
  *  Provides:
  *    - LOG_Init()            : USART1 @ 115200 8N1, TX/RX
  *    - HAL_UART_MspInit()    : strong override (GPIO + clock for USART1)
  *    - printf retarget       : fputc() -> blocking HAL_UART_Transmit, no semihosting
  *
  *  USART1 clock source defaults to APB2 (rcc_pclk2). Baud rate is derived by
  *  the HAL from the configured peripheral clock, so no explicit
  *  __HAL_RCC_USART1_CONFIG() is required for the reset-default selection.
  ******************************************************************************
  */

#include "usbnic_log.h"
#include <stdarg.h>

UART_HandleTypeDef hlog_uart;


/* Largest single log line we will emit. Lines longer than this are truncated
   (the trailing CRLF is always preserved). Lives on the caller's stack, not as
   a shared static, so concurrent callers from different tasks cannot scribble
   over each other's buffer mid-format. */
#define LOG_LINE_MAX  128U

void LOG_Printf(const char *fmt, ...)
{
  char    line[LOG_LINE_MAX];
  va_list ap;
  int     n;

  /* Timestamp prefix, identical format to the old printf-based macro. */
  n = snprintf(line, sizeof(line), "[%8lu] ", (unsigned long)HAL_GetTick());
  if ((n < 0) || (n >= (int)sizeof(line)))
  {
    n = (int)sizeof(line) - 1;          /* prefix alone overflowed (shouldn't) */
  }

  va_start(ap, fmt);
  {
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    if (m > 0)
    {
      n += m;
      if (n > (int)sizeof(line) - 1)    /* vsnprintf reports the untruncated len */
      {
        n = (int)sizeof(line) - 1;
      }
    }
  }
  va_end(ap);

  /* Reserve two bytes for CRLF; overwrite the tail if the line filled up. */
  if (n > (int)sizeof(line) - 2)
  {
    n = (int)sizeof(line) - 2;
  }
  line[n++] = '\r';
  line[n++] = '\n';

  HAL_UART_Transmit(&hlog_uart, (uint8_t *)line, (uint16_t)n, HAL_MAX_DELAY);
}

void LOG_Init(void)
{
  hlog_uart.Instance                    = USART1;
  hlog_uart.Init.BaudRate               = 115200;
  hlog_uart.Init.WordLength             = UART_WORDLENGTH_8B;
  hlog_uart.Init.StopBits               = UART_STOPBITS_1;
  hlog_uart.Init.Parity                 = UART_PARITY_NONE;
  hlog_uart.Init.Mode                   = UART_MODE_TX_RX;
  hlog_uart.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
  hlog_uart.Init.OverSampling           = UART_OVERSAMPLING_16;
  hlog_uart.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
  hlog_uart.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
  hlog_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&hlog_uart) != HAL_OK)
  {
    /* Nothing we can usefully do this early; just continue without logs. */
  }
}

/* Strong override of the HAL weak MSP init: routes USART1 to PA9/PA10 (AF7). */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef gpio = {0};

  if (huart->Instance == USART1)
  {
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 = USART1_TX, PA10 = USART1_RX */
    gpio.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    __HAL_RCC_USART1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_10);
  }
}

/* ------------------------------------------------------------------------- */
/* printf retarget (Arm Compiler 6, no semihosting)                          */
/* ------------------------------------------------------------------------- */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)

__asm(".global __use_no_semihosting\n\t");

FILE __stdout;

void _sys_exit(int x)
{
  (void)x;
  while (1) { }
}

void _ttywrch(int ch)
{
  (void)ch;
}

char *_sys_command_string(char *cmd, int len)
{
  (void)cmd;
  (void)len;
  return 0;
}

int fputc(int ch, FILE *f)
{
  uint8_t c = (uint8_t)ch;
  (void)f;
  HAL_UART_Transmit(&hlog_uart, &c, 1, 0xFFFF);
  return ch;
}

#endif /* __ARMCC_VERSION */
