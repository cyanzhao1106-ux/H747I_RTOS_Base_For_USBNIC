/**
  ******************************************************************************
  * @file    usbh_rtknic_reg.c
  * @author  USB NIC integration
  * @brief   PLA / USB OCP register access over EP0 vendor control transfers for
  *          the Realtek USB NIC family (RTL8152B/8153B/8156B/8157/8159).
  *
  *  Realtek USB LAN parts have two on-chip MCUs, each owning a register space
  *  reached through a single vendor request (bRequest = 0x05) on the default
  *  control endpoint:
  *    - PLA OCP : Physical Layer Access MCU  (Ethernet MAC / PHY features)
  *    - USB OCP : USB MCU                     (USB-side features)
  *
  *  Vendor setup packet (per the USBNIC_Vendor_RW spec, mirrors Linux r8152):
  *    bmRequestType : 0x40 write (Host->Dev) / 0xC0 read (Dev->Host), vendor, device
  *    bRequest      : 0x05
  *    wValue        : start address, MUST be 4-byte aligned
  *    wIndex        : bit 8     = register space (0 = USB OCP, 1 = PLA OCP)
  *                    bits 7..0 = byte-enable mask
  *                                bits 3..0 -> bytes of the first DWORD
  *                                bits 7..4 -> bytes of the last  DWORD
  *    wLength       : byte count, MUST be a multiple of 4, <= 512 per transfer
  *
  *  The byte-enable mask must mark exactly the bytes of interest for BOTH reads
  *  and writes - the hardware may misbehave if bytes outside the intended range
  *  are enabled, even on a read. The typed word/byte helpers below align the
  *  address down to its DWORD and set the mask accordingly.
  *
  *  Implementation note: these helpers drive the standard ST USBH_CtlReq state
  *  machine to completion in a bounded poll loop. The OTG host interrupt
  *  advances the URB state independently (it preempts the USB host task), and
  *  USBH_OS_PutMessage() drops events when its queue is full, so pumping
  *  USBH_CtlReq from the USB host task is safe. They BLOCK, so call them only
  *  from that task context (a protocol setup() step or USBH_RTKNIC_OnActive()),
  *  never from an ISR or concurrently with the running data path.
  *
  *  ---------------------------------------------------------------------------
  *  Usage
  *  ---------------------------------------------------------------------------
  *  Pick the register space with 'type':
  *      RTKNIC_MCU_TYPE_PLA  - Ethernet MAC / PHY registers (PLA OCP)
  *      RTKNIC_MCU_TYPE_USB  - USB-side registers            (USB OCP)
  *
  *  Typed accessors (most common - one register at a time). 'addr' alignment:
  *  dword 4-byte, word 2-byte, byte any. They return USBH_OK / USBH_FAIL:
  *
  *      uint32_t v;
  *      if (USBH_RTKNIC_OcpReadDword(phost, RTKNIC_MCU_TYPE_PLA, 0xC000, &v) == USBH_OK)
  *      {
  *        v |= 0x1U;
  *        (void)USBH_RTKNIC_OcpWriteDword(phost, RTKNIC_MCU_TYPE_PLA, 0xC000, v);
  *      }
  *
  *      uint16_t w;
  *      (void)USBH_RTKNIC_OcpReadWord (phost, RTKNIC_MCU_TYPE_USB, 0xB400, &w);
  *      (void)USBH_RTKNIC_OcpWriteByte(phost, RTKNIC_MCU_TYPE_PLA, 0xD000, 0x55U);
  *
  *  Block helpers (contiguous register window; 'addr' 4-byte aligned, 'len' a
  *  multiple of 4, data little-endian):
  *
  *      uint8_t buf[16];
  *      (void)USBH_RTKNIC_OcpRead (phost, RTKNIC_MCU_TYPE_PLA, 0xC100, buf, sizeof(buf));
  *      (void)USBH_RTKNIC_OcpWrite(phost, RTKNIC_MCU_TYPE_PLA, 0xC100, buf, sizeof(buf));
  *
  *  Where to call from - the USB host task context only. The natural hook for
  *  per-platform Tx/Rx tuning is USBH_RTKNIC_OnActive(), which the core class
  *  invokes once after the device reaches the configured CDC state:
  *
  *      void USBH_RTKNIC_OnActive(USBH_HandleTypeDef *phost)
  *      {
  *        // RTL8156B example: read-modify-write a PLA register
  *        uint32_t v;
  *        if (USBH_RTKNIC_OcpReadDword(phost, RTKNIC_MCU_TYPE_PLA, 0xC0XX, &v) == USBH_OK)
  *        {
  *          v = (v & ~MASK) | TUNED_VALUE;
  *          (void)USBH_RTKNIC_OcpWriteDword(phost, RTKNIC_MCU_TYPE_PLA, 0xC0XX, v);
  *        }
  *      }
  ******************************************************************************
  */

/* Includes -----------------------------------------------------------------*/
#include "usbh_rtknic.h"
#include "usbnic_log.h"

/* Worst-case time budget to pump one EP0 control transfer to completion.     */
#define RTKNIC_OCP_TIMEOUT_MS   500U

/* Private functions --------------------------------------------------------*/

/**
  * @brief  Drive a single control request to completion (blocking).
  * @note   Must run in the USB host task context; relies on the OTG ISR to
  *         advance the URB state. Restores RequestState on timeout so a later
  *         call starts cleanly.
  * @retval USBH_OK on success, USBH_FAIL on failure or timeout.
  */
static USBH_StatusTypeDef RTKNIC_CtlReqSync(USBH_HandleTypeDef *phost,
                                            uint8_t *buff, uint16_t length)
{
  uint32_t t0 = HAL_GetTick();
  USBH_StatusTypeDef st;

  do
  {
    st = USBH_CtlReq(phost, buff, length);
    if (st != USBH_BUSY)
    {
      break;
    }
  } while ((HAL_GetTick() - t0) < RTKNIC_OCP_TIMEOUT_MS);

  if (st == USBH_BUSY)
  {
    /* Timed out mid-transfer: abandon and re-arm the control state machine. */
    phost->RequestState = CMD_SEND;
    phost->Control.state = CTRL_IDLE;
    st = USBH_FAIL;
  }

  return st;
}

/**
  * @brief  Issue one OCP vendor control transfer (single chunk).
  * @param  dir_in  1 = read (Dev->Host), 0 = write (Host->Dev)
  * @param  type    RTKNIC_MCU_TYPE_PLA or RTKNIC_MCU_TYPE_USB
  * @param  addr    4-byte aligned start address (caller-checked)
  * @param  byteen  byte-enable mask for the setup wIndex low byte
  * @param  data    little-endian data buffer (len bytes)
  * @param  len     multiple of 4, <= RTKNIC_OCP_MAX_CHUNK (caller-checked)
  */
static USBH_StatusTypeDef RTKNIC_OcpXfer(USBH_HandleTypeDef *phost, uint8_t dir_in,
                                         uint16_t type, uint16_t addr, uint8_t byteen,
                                         uint8_t *data, uint16_t len)
{
  /* OTG internal DMA requires every EP0 data buffer to be 4-byte aligned. The
     typed accessors pass small stack arrays and block callers may pass any
     alignment, so bounce every chunk through one word-aligned scratch buffer.
     A single static is safe here: these helpers run only in the (single) USB
     host task context and block to completion (see the file header). */
  static uint32_t xfer_buf32[RTKNIC_OCP_MAX_CHUNK / 4U];
  uint8_t *xbuf = (uint8_t *)xfer_buf32;
  USBH_StatusTypeDef st;

  if (len > (uint16_t)sizeof(xfer_buf32))
  {
    return USBH_FAIL;
  }

  if (dir_in == 0U)
  {
    /* OUT: stage the caller's payload into the aligned buffer. */
    (void)USBH_memcpy(xbuf, data, len);
  }

  phost->Control.setup.b.bmRequestType =
      (uint8_t)((dir_in != 0U) ? USB_D2H : USB_H2D) |
      USB_REQ_TYPE_VENDOR | USB_REQ_RECIPIENT_DEVICE;
  phost->Control.setup.b.bRequest  = RTKNIC_REQ_VENDOR_OCP;
  phost->Control.setup.b.wValue.w  = addr;
  phost->Control.setup.b.wIndex.w  = (uint16_t)(type | (uint16_t)byteen);
  phost->Control.setup.b.wLength.w = len;

  st = RTKNIC_CtlReqSync(phost, xbuf, len);

  if ((dir_in != 0U) && (st == USBH_OK))
  {
    /* IN: copy the received register data back to the caller. */
    (void)USBH_memcpy(data, xbuf, len);
  }

  return st;
}

/* Exported block helpers ---------------------------------------------------*/

USBH_StatusTypeDef USBH_RTKNIC_OcpRead(USBH_HandleTypeDef *phost, uint16_t type,
                                       uint16_t addr, void *data, uint16_t len)
{
  uint8_t *p = (uint8_t *)data;

  if ((phost == NULL) || (data == NULL) || (len == 0U) ||
      ((addr & 3U) != 0U) || ((len & 3U) != 0U))
  {
    return USBH_FAIL;
  }

  while (len != 0U)
  {
    uint16_t chunk = (len > RTKNIC_OCP_MAX_CHUNK) ? RTKNIC_OCP_MAX_CHUNK : len;
    USBH_StatusTypeDef st = RTKNIC_OcpXfer(phost, 1U, type, addr,
                                           RTKNIC_BYTE_EN_DWORD, p, chunk);
    if (st != USBH_OK)
    {
      LOG("RTKNIC: OCP read %s @0x%04X failed st=%d",
          (type == RTKNIC_MCU_TYPE_PLA) ? "PLA" : "USB", (unsigned)addr, (int)st);
      return st;
    }
    addr += chunk;
    p    += chunk;
    len  -= chunk;
  }

  return USBH_OK;
}

USBH_StatusTypeDef USBH_RTKNIC_OcpWrite(USBH_HandleTypeDef *phost, uint16_t type,
                                        uint16_t addr, const void *data, uint16_t len)
{
  /* USBH_CtlReq does not modify the buffer on an OUT transfer; cast is safe. */
  uint8_t *p = (uint8_t *)(uint32_t)data;

  if ((phost == NULL) || (data == NULL) || (len == 0U) ||
      ((addr & 3U) != 0U) || ((len & 3U) != 0U))
  {
    return USBH_FAIL;
  }

  while (len != 0U)
  {
    uint16_t chunk = (len > RTKNIC_OCP_MAX_CHUNK) ? RTKNIC_OCP_MAX_CHUNK : len;
    USBH_StatusTypeDef st = RTKNIC_OcpXfer(phost, 0U, type, addr,
                                           RTKNIC_BYTE_EN_DWORD, p, chunk);
    if (st != USBH_OK)
    {
      LOG("RTKNIC: OCP write %s @0x%04X failed st=%d",
          (type == RTKNIC_MCU_TYPE_PLA) ? "PLA" : "USB", (unsigned)addr, (int)st);
      return st;
    }
    addr += chunk;
    p    += chunk;
    len  -= chunk;
  }

  return USBH_OK;
}

/* Exported typed accessors -------------------------------------------------*/

USBH_StatusTypeDef USBH_RTKNIC_OcpReadDword(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint32_t *val)
{
  uint8_t b[4];
  USBH_StatusTypeDef st;

  if ((phost == NULL) || (val == NULL) || ((addr & 3U) != 0U))
  {
    return USBH_FAIL;
  }

  st = RTKNIC_OcpXfer(phost, 1U, type, addr, RTKNIC_BYTE_EN_DWORD, b, 4U);
  if (st == USBH_OK)
  {
    *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  }
  return st;
}

USBH_StatusTypeDef USBH_RTKNIC_OcpWriteDword(USBH_HandleTypeDef *phost, uint16_t type,
                                             uint16_t addr, uint32_t val)
{
  uint8_t b[4];

  if ((phost == NULL) || ((addr & 3U) != 0U))
  {
    return USBH_FAIL;
  }

  b[0] = (uint8_t)val;
  b[1] = (uint8_t)(val >> 8);
  b[2] = (uint8_t)(val >> 16);
  b[3] = (uint8_t)(val >> 24);

  return RTKNIC_OcpXfer(phost, 0U, type, addr, RTKNIC_BYTE_EN_DWORD, b, 4U);
}

USBH_StatusTypeDef USBH_RTKNIC_OcpReadWord(USBH_HandleTypeDef *phost, uint16_t type,
                                           uint16_t addr, uint16_t *val)
{
  uint8_t b[4];
  uint16_t base  = (uint16_t)(addr & ~3U);
  uint8_t  shift = (uint8_t)(addr & 2U);                       /* 0 or 2 bytes */
  uint8_t  byteen = (uint8_t)(RTKNIC_BYTE_EN_WORD << shift);   /* 0x33 / 0xCC  */
  USBH_StatusTypeDef st;

  if ((phost == NULL) || (val == NULL) || ((addr & 1U) != 0U))
  {
    return USBH_FAIL;
  }

  st = RTKNIC_OcpXfer(phost, 1U, type, base, byteen, b, 4U);
  if (st == USBH_OK)
  {
    *val = (uint16_t)((uint16_t)b[shift] | ((uint16_t)b[shift + 1U] << 8));
  }
  return st;
}

USBH_StatusTypeDef USBH_RTKNIC_OcpWriteWord(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint16_t val)
{
  uint8_t b[4] = {0U, 0U, 0U, 0U};
  uint16_t base  = (uint16_t)(addr & ~3U);
  uint8_t  shift = (uint8_t)(addr & 2U);
  uint8_t  byteen = (uint8_t)(RTKNIC_BYTE_EN_WORD << shift);

  if ((phost == NULL) || ((addr & 1U) != 0U))
  {
    return USBH_FAIL;
  }

  /* Only the enabled bytes are written; the rest of b[] is ignored by HW. */
  b[shift]      = (uint8_t)val;
  b[shift + 1U] = (uint8_t)(val >> 8);

  return RTKNIC_OcpXfer(phost, 0U, type, base, byteen, b, 4U);
}

USBH_StatusTypeDef USBH_RTKNIC_OcpReadByte(USBH_HandleTypeDef *phost, uint16_t type,
                                           uint16_t addr, uint8_t *val)
{
  uint8_t b[4];
  uint16_t base  = (uint16_t)(addr & ~3U);
  uint8_t  shift = (uint8_t)(addr & 3U);                       /* 0..3 bytes  */
  uint8_t  byteen = (uint8_t)(RTKNIC_BYTE_EN_BYTE << shift);   /* 0x11<<shift */
  USBH_StatusTypeDef st;

  if ((phost == NULL) || (val == NULL))
  {
    return USBH_FAIL;
  }

  st = RTKNIC_OcpXfer(phost, 1U, type, base, byteen, b, 4U);
  if (st == USBH_OK)
  {
    *val = b[shift];
  }
  return st;
}

USBH_StatusTypeDef USBH_RTKNIC_OcpWriteByte(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint8_t val)
{
  uint8_t b[4] = {0U, 0U, 0U, 0U};
  uint16_t base  = (uint16_t)(addr & ~3U);
  uint8_t  shift = (uint8_t)(addr & 3U);
  uint8_t  byteen = (uint8_t)(RTKNIC_BYTE_EN_BYTE << shift);

  if (phost == NULL)
  {
    return USBH_FAIL;
  }

  b[shift] = val;

  return RTKNIC_OcpXfer(phost, 0U, type, base, byteen, b, 4U);
}
