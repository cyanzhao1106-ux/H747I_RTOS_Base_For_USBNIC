/**
  ******************************************************************************
  * @file    usbh_rtknic_ncm.c
  * @author  USB NIC integration
  * @brief   CDC-NCM (Network Control Model) protocol module for the Realtek
  *          USB NIC host class. Offered by RTL8156B / 8157 / 8159.
  *
  *          Unlike ECM (one bare Ethernet frame per bulk transfer), NCM
  *          multiplexes one or more Ethernet datagrams into an NCM Transfer
  *          Block (NTB). This module implements the 16-bit NTB format (NTB-16),
  *          which is the mandatory format every NCM function supports:
  *
  *            NTB-16 = NTH16 + one or more NDP16 + the datagrams.
  *
  *            NTH16  (NCM Transfer Header, 12 bytes)
  *              dwSignature   "NCMH"
  *              wHeaderLength  = 12
  *              wSequence
  *              wBlockLength   = total NTB length
  *              wNdpIndex      = byte offset of the first NDP16
  *            NDP16  (NCM Datagram Pointer, >= 16 bytes)
  *              dwSignature   "NCM0" (no CRC) / "NCM1" (CRC)
  *              wLength        = length of this NDP16
  *              wNextNdpIndex  = offset of next NDP16 (0 = last)
  *              (wDatagramIndex, wDatagramLength) pairs, (0,0)-terminated
  *
  *          On RX we walk every NDP16 and deliver each datagram upward. On TX
  *          we wrap a single Ethernet frame in a minimal one-datagram NTB-16.
  *
  *          Bring-up: read the NTB parameters, cap the device's NTB-IN size to
  *          our RX buffer, enable the receive packet filter and read the MAC
  *          from the iMACAddress string descriptor (same functional descriptor
  *          as ECM). Every control step tolerates failure so an
  *          unusual/partial implementation still yields a usable link.
  ******************************************************************************
  */

/* Includes -----------------------------------------------------------------*/
#include "usbh_rtknic.h"

/* NCM bring-up sub-states (stored in h->proto_state) */
#define NCM_SETUP_GET_PARAMS     0U
#define NCM_SETUP_SET_INPUT_SIZE 1U
#define NCM_SETUP_SET_FILTER     2U
#define NCM_SETUP_GET_MAC        3U
#define NCM_SETUP_FINISH         4U

/* Receive filter: directed + broadcast + all-multicast (CDC ECM 6.2.4) */
#define NCM_PACKET_FILTER        (RTKNIC_PKT_FILTER_DIRECTED | \
                                  RTKNIC_PKT_FILTER_BROADCAST | \
                                  RTKNIC_PKT_FILTER_ALL_MULTICAST)

/* NTB-16 fixed sizes / offsets */
#define NCM_NTH16_LEN            12U   /* NCM Transfer Header (16-bit)         */
#define NCM_NDP16_MIN_LEN        16U   /* NDP16 with 1 datagram + (0,0) term   */
#define NCM_NTB_PARAMS_LEN       28U   /* NTB Parameter Structure              */

/* TX layout: NTH16 | NDP16(1 datagram) | datagram, all 4-byte aligned.       */
#define NCM_TX_NDP_OFFSET        NCM_NTH16_LEN                         /* 12   */
#define NCM_TX_DGRAM_OFFSET      (NCM_NTH16_LEN + NCM_NDP16_MIN_LEN)   /* 28   */

/* h->priv[] usage for the NCM protocol */
#define NCM_PRIV_TX_SEQ          0U    /* outgoing NTB sequence number         */
#define NCM_PRIV_IN_MAX          1U    /* device-reported dwNtbInMaxSize       */

/* Little-endian field accessors (NTB fields are unaligned in the byte stream) */
static uint16_t ncm_rd16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ncm_rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ncm_wr16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/* NTH16 signature "NCMH" (datagram-header, 16-bit). */
static uint8_t ncm_is_nth16_sig(const uint8_t *p)
{
  return (uint8_t)((p[0] == (uint8_t)'N') && (p[1] == (uint8_t)'C') &&
                   (p[2] == (uint8_t)'M') && (p[3] == (uint8_t)'H'));
}

/* NDP16 signature "NCM0" (no CRC) or "NCM1" (per-datagram CRC). */
static uint8_t ncm_is_ndp16_sig(const uint8_t *p)
{
  return (uint8_t)((p[0] == (uint8_t)'N') && (p[1] == (uint8_t)'C') &&
                   (p[2] == (uint8_t)'M') &&
                   ((p[3] == (uint8_t)'0') || (p[3] == (uint8_t)'1')));
}

/* MAC helpers (shared semantics with the ECM module). ----------------------*/

static uint8_t ncm_hexval(uint8_t c)
{
  if ((c >= (uint8_t)'0') && (c <= (uint8_t)'9')) { return (uint8_t)(c - (uint8_t)'0'); }
  if ((c >= (uint8_t)'a') && (c <= (uint8_t)'f')) { return (uint8_t)(c - (uint8_t)'a' + 10U); }
  if ((c >= (uint8_t)'A') && (c <= (uint8_t)'F')) { return (uint8_t)(c - (uint8_t)'A' + 10U); }
  return 0xFFU;
}

static uint8_t ncm_parse_mac(const uint8_t *str, uint8_t *mac)
{
  uint8_t i;

  for (i = 0U; i < RTKNIC_MAC_ADDR_LEN; i++)
  {
    uint8_t hi = ncm_hexval(str[i * 2U]);
    uint8_t lo = ncm_hexval(str[(i * 2U) + 1U]);
    if ((hi == 0xFFU) || (lo == 0xFFU))
    {
      return 0U;
    }
    mac[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1U;
}

static void ncm_fallback_mac(uint8_t *mac)
{
#if defined(UID_BASE)
  const uint32_t *uid = (const uint32_t *)UID_BASE;
  uint32_t a = uid[0] ^ uid[1] ^ uid[2];
  mac[0] = 0x02U;                 /* locally administered, unicast */
  mac[1] = 0x00U;
  mac[2] = (uint8_t)(a >> 24);
  mac[3] = (uint8_t)(a >> 16);
  mac[4] = (uint8_t)(a >> 8);
  mac[5] = (uint8_t)(a);
#else
  mac[0] = 0x02U; mac[1] = 0x00U; mac[2] = 0x00U;
  mac[3] = 0x52U; mac[4] = 0x54U; mac[5] = 0x4EU;   /* 'R','T','N' */
#endif
}

/* Protocol operations ------------------------------------------------------*/

static USBH_StatusTypeDef NCM_Setup(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  USBH_StatusTypeDef req;
  /* ntb_params / in_size_buf are handed straight to USBH_CtlReq, whose EP0 data
     stage uses OTG internal DMA (buffers must be 4-byte aligned). mac_str is
     filled by USBH_Get_StringDesc via CPU copy, so it needs no alignment. */
  static uint8_t ntb_params[NCM_NTB_PARAMS_LEN] __attribute__((aligned(4)));
  static uint8_t in_size_buf[4] __attribute__((aligned(4)));
  static uint8_t mac_str[32];

  switch (h->proto_state)
  {
    case NCM_SETUP_GET_PARAMS:
      /* GET_NTB_PARAMETERS (D2H, class, interface). Tolerate failure; the
         spec guarantees a minimum NTB-IN size of 2048, which we request. */
      phost->Control.setup.b.bmRequestType = USB_D2H | USB_REQ_TYPE_CLASS |
                                             USB_REQ_RECIPIENT_INTERFACE;
      phost->Control.setup.b.bRequest = RTKNIC_REQ_GET_NTB_PARAMETERS;
      phost->Control.setup.b.wValue.w = 0U;
      phost->Control.setup.b.wIndex.w = h->CommItfNum;
      phost->Control.setup.b.wLength.w = NCM_NTB_PARAMS_LEN;

      req = USBH_CtlReq(phost, ntb_params, NCM_NTB_PARAMS_LEN);
      if (req == USBH_OK)
      {
        /* dwNtbInMaxSize lives at offset 4 of the parameter structure. */
        h->priv[NCM_PRIV_IN_MAX] = ncm_rd32(&ntb_params[4]);
        h->proto_state = NCM_SETUP_SET_INPUT_SIZE;
      }
      else if (req != USBH_BUSY)
      {
        h->priv[NCM_PRIV_IN_MAX] = 0U;
        h->proto_state = NCM_SETUP_SET_INPUT_SIZE;
      }
      return USBH_BUSY;

    case NCM_SETUP_SET_INPUT_SIZE:
    {
      /* Cap the device's NTB-IN size to our RX buffer so a whole NTB always
         fits in one bulk transfer. Clamp to the device maximum if known. */
      uint32_t want = RTKNIC_RX_BUF_SIZE;
      uint32_t dev_max = h->priv[NCM_PRIV_IN_MAX];
      if ((dev_max != 0U) && (dev_max < want))
      {
        want = dev_max;
      }
      in_size_buf[0] = (uint8_t)(want & 0xFFU);
      in_size_buf[1] = (uint8_t)((want >> 8) & 0xFFU);
      in_size_buf[2] = (uint8_t)((want >> 16) & 0xFFU);
      in_size_buf[3] = (uint8_t)((want >> 24) & 0xFFU);

      phost->Control.setup.b.bmRequestType = USB_H2D | USB_REQ_TYPE_CLASS |
                                             USB_REQ_RECIPIENT_INTERFACE;
      phost->Control.setup.b.bRequest = RTKNIC_REQ_SET_NTB_INPUT_SIZE;
      phost->Control.setup.b.wValue.w = 0U;
      phost->Control.setup.b.wIndex.w = h->CommItfNum;
      phost->Control.setup.b.wLength.w = 4U;

      req = USBH_CtlReq(phost, in_size_buf, 4U);
      if ((req == USBH_OK) || ((req != USBH_BUSY)))
      {
        /* Success or a tolerated failure: move on (default NTB-IN size still
           applies if the device rejected the request). */
        h->proto_state = NCM_SETUP_SET_FILTER;
      }
      return USBH_BUSY;
    }

    case NCM_SETUP_SET_FILTER:
      phost->Control.setup.b.bmRequestType = USB_H2D | USB_REQ_TYPE_CLASS |
                                             USB_REQ_RECIPIENT_INTERFACE;
      phost->Control.setup.b.bRequest = RTKNIC_REQ_SET_ETH_PACKET_FILTER;
      phost->Control.setup.b.wValue.w = NCM_PACKET_FILTER;
      phost->Control.setup.b.wIndex.w = h->CommItfNum;
      phost->Control.setup.b.wLength.w = 0U;

      req = USBH_CtlReq(phost, NULL, 0U);
      if ((req == USBH_OK) || (req != USBH_BUSY))
      {
        /* Tolerate a filter failure - many dongles default to receiving. */
        h->proto_state = (h->imac_str_idx != 0U) ? NCM_SETUP_GET_MAC : NCM_SETUP_FINISH;
      }
      return USBH_BUSY;

    case NCM_SETUP_GET_MAC:
      req = USBH_Get_StringDesc(phost, h->imac_str_idx, mac_str, sizeof(mac_str));
      if (req == USBH_OK)
      {
        if (ncm_parse_mac(mac_str, h->mac_addr) != 0U)
        {
          h->mac_valid = 1U;
        }
        h->proto_state = NCM_SETUP_FINISH;
      }
      else if (req != USBH_BUSY)
      {
        h->proto_state = NCM_SETUP_FINISH;
      }
      return USBH_BUSY;

    case NCM_SETUP_FINISH:
    default:
      if (h->mac_valid == 0U)
      {
        ncm_fallback_mac(h->mac_addr);
        h->mac_valid = 1U;
      }
      h->priv[NCM_PRIV_TX_SEQ] = 0U;
      return USBH_OK;
  }
}

/**
  * @brief  Parse a received NTB-16 and deliver each contained datagram.
  */
static void NCM_RxUnwrap(USBH_HandleTypeDef *phost, uint8_t *buf, uint32_t len)
{
  uint32_t block_len;
  uint32_t ndp_off;
  uint8_t  ndp_guard;

  /* Need at least an NTH16, and it must be NTB-16 ("NCMH"). */
  if ((len < NCM_NTH16_LEN) || (ncm_is_nth16_sig(buf) == 0U))
  {
    return;
  }

  /* Trust the smaller of the declared block length and what we actually got. */
  block_len = ncm_rd16(&buf[8]);          /* wBlockLength (NTB-16)            */
  if ((block_len == 0U) || (block_len > len))
  {
    block_len = len;
  }

  ndp_off = ncm_rd16(&buf[10]);           /* wNdpIndex                        */

  /* Walk the (possibly chained) NDP16 list. Bounded to avoid malformed loops. */
  for (ndp_guard = 0U; ndp_guard < 16U; ndp_guard++)
  {
    uint32_t ndp_len;
    uint32_t entry;
    uint32_t next;

    if ((ndp_off == 0U) || ((ndp_off + 8U) > block_len))
    {
      break;
    }
    if (ncm_is_ndp16_sig(&buf[ndp_off]) == 0U)
    {
      break;
    }

    ndp_len = ncm_rd16(&buf[ndp_off + 4U]);     /* wLength                    */
    next    = ncm_rd16(&buf[ndp_off + 6U]);     /* wNextNdpIndex              */

    if ((ndp_len < NCM_NDP16_MIN_LEN) || ((ndp_off + ndp_len) > block_len))
    {
      break;
    }

    /* (wDatagramIndex, wDatagramLength) pairs follow the 8-byte NDP header,
       terminated by a (0,0) entry. */
    for (entry = ndp_off + 8U; (entry + 4U) <= (ndp_off + ndp_len); entry += 4U)
    {
      uint32_t dg_idx = ncm_rd16(&buf[entry]);
      uint32_t dg_len = ncm_rd16(&buf[entry + 2U]);

      if ((dg_idx == 0U) && (dg_len == 0U))
      {
        break;                                  /* terminator                 */
      }
      if ((dg_len >= 14U) && ((dg_idx + dg_len) <= block_len))
      {
        USBH_RTKNIC_DeliverFrame(phost, &buf[dg_idx], dg_len);
      }
    }

    /* Advance to the next NDP; require forward progress to stay bounded. */
    if ((next == 0U) || (next <= ndp_off))
    {
      break;
    }
    ndp_off = next;
  }
}

/**
  * @brief  Wrap a single Ethernet frame into a minimal one-datagram NTB-16.
  * @note   Fixed 4-byte-aligned layout (NTH16 @0, NDP16 @12, datagram @28).
  *         This satisfies the standard NCM OUT alignment (divisor/alignment 4,
  *         remainder 0) reported by the Realtek parts.
  */
static uint32_t NCM_TxWrap(USBH_HandleTypeDef *phost, const uint8_t *frame, uint32_t len)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  uint8_t *buf = h->tx_buf;
  uint32_t total = NCM_TX_DGRAM_OFFSET + len;
  uint16_t seq = (uint16_t)h->priv[NCM_PRIV_TX_SEQ];

  /* Defensive: the caller bounds len to RTKNIC_ETH_MAX_FRAME (1514), so total
     (<= 1542) always fits RTKNIC_TX_BUF_SIZE; clamp regardless. */
  if (total > RTKNIC_TX_BUF_SIZE)
  {
    len = RTKNIC_TX_BUF_SIZE - NCM_TX_DGRAM_OFFSET;
    total = RTKNIC_TX_BUF_SIZE;
  }

  (void)USBH_memset(buf, 0, NCM_TX_DGRAM_OFFSET);

  /* NTH16 */
  buf[0] = (uint8_t)'N'; buf[1] = (uint8_t)'C';
  buf[2] = (uint8_t)'M'; buf[3] = (uint8_t)'H';
  ncm_wr16(&buf[4],  NCM_NTH16_LEN);          /* wHeaderLength = 12           */
  ncm_wr16(&buf[6],  seq);                    /* wSequence                    */
  ncm_wr16(&buf[8],  (uint16_t)total);        /* wBlockLength                 */
  ncm_wr16(&buf[10], NCM_TX_NDP_OFFSET);      /* wNdpIndex = 12               */

  /* NDP16 (one datagram + null terminator) */
  buf[NCM_TX_NDP_OFFSET + 0] = (uint8_t)'N';
  buf[NCM_TX_NDP_OFFSET + 1] = (uint8_t)'C';
  buf[NCM_TX_NDP_OFFSET + 2] = (uint8_t)'M';
  buf[NCM_TX_NDP_OFFSET + 3] = (uint8_t)'0';  /* no CRC                       */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 4], NCM_NDP16_MIN_LEN);  /* wLength = 16   */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 6], 0U);                 /* wNextNdpIndex  */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 8],  NCM_TX_DGRAM_OFFSET);   /* dgram idx  */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 10], (uint16_t)len);        /* dgram len   */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 12], 0U);                   /* term idx    */
  ncm_wr16(&buf[NCM_TX_NDP_OFFSET + 14], 0U);                   /* term len    */

  /* Datagram payload */
  (void)USBH_memcpy(&buf[NCM_TX_DGRAM_OFFSET], frame, len);

  h->priv[NCM_PRIV_TX_SEQ] = (uint32_t)((seq + 1U) & 0xFFFFU);

  /* A bulk transfer whose length is a multiple of the OUT max packet size
     needs a terminating zero-length packet to mark the end of the NTB. */
  h->tx_zlp = ((h->OutEpSize != 0U) && ((total % h->OutEpSize) == 0U)) ? 1U : 0U;

  return total;
}

const RTKNIC_ProtoOpsTypeDef RTKNIC_NCM_Ops =
{
  RTKNIC_PROTO_NCM,
  RTKNIC_CDC_SUBCLASS_NCM,
  NCM_Setup,
  NCM_RxUnwrap,
  NCM_TxWrap,
};
