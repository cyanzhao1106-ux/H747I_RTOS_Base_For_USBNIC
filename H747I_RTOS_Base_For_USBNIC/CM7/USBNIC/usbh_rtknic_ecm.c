/**
  ******************************************************************************
  * @file    usbh_rtknic_ecm.c
  * @author  USB NIC integration
  * @brief   CDC-ECM protocol module for the Realtek USB NIC host class.
  *
  *          ECM carries one bare Ethernet frame (no FCS) per bulk transfer,
  *          terminated by a short / zero-length packet. Bring-up consists of
  *          enabling the receive packet filter and reading the MAC address
  *          from the iMACAddress string descriptor.
  ******************************************************************************
  */

/* Includes -----------------------------------------------------------------*/
#include "usbh_rtknic.h"

/* ECM bring-up sub-states (stored in h->proto_state) */
#define ECM_SETUP_SET_FILTER   0U
#define ECM_SETUP_GET_MAC      1U
#define ECM_SETUP_FINISH       2U

/* Receive filter: directed + broadcast + all-multicast */
#define ECM_PACKET_FILTER      (RTKNIC_PKT_FILTER_DIRECTED | \
                                RTKNIC_PKT_FILTER_BROADCAST | \
                                RTKNIC_PKT_FILTER_ALL_MULTICAST)

/* Private helpers ----------------------------------------------------------*/

static uint8_t ecm_hexval(uint8_t c)
{
  if ((c >= (uint8_t)'0') && (c <= (uint8_t)'9'))
  {
    return (uint8_t)(c - (uint8_t)'0');
  }
  if ((c >= (uint8_t)'a') && (c <= (uint8_t)'f'))
  {
    return (uint8_t)(c - (uint8_t)'a' + 10U);
  }
  if ((c >= (uint8_t)'A') && (c <= (uint8_t)'F'))
  {
    return (uint8_t)(c - (uint8_t)'A' + 10U);
  }
  return 0xFFU;
}

/* Parse a 12-character ASCII hex MAC string into 6 bytes. */
static uint8_t ecm_parse_mac(const uint8_t *str, uint8_t *mac)
{
  uint8_t i;

  for (i = 0U; i < RTKNIC_MAC_ADDR_LEN; i++)
  {
    uint8_t hi = ecm_hexval(str[i * 2U]);
    uint8_t lo = ecm_hexval(str[(i * 2U) + 1U]);
    if ((hi == 0xFFU) || (lo == 0xFFU))
    {
      return 0U;
    }
    mac[i] = (uint8_t)((hi << 4) | lo);
  }
  return 1U;
}

/* Derive a stable locally-administered MAC from the STM32 unique ID, used as
   a fallback when the device exposes no usable iMACAddress string. */
static void ecm_fallback_mac(uint8_t *mac)
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
  mac[3] = 0x52U; mac[4] = 0x54U; mac[5] = 0x4BU;
#endif
}

/* Protocol operations ------------------------------------------------------*/

static USBH_StatusTypeDef ECM_Setup(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  USBH_StatusTypeDef req;
  static uint8_t mac_str[32];

  switch (h->proto_state)
  {
    case ECM_SETUP_SET_FILTER:
      phost->Control.setup.b.bmRequestType = USB_H2D | USB_REQ_TYPE_CLASS |
                                             USB_REQ_RECIPIENT_INTERFACE;
      phost->Control.setup.b.bRequest = RTKNIC_REQ_SET_ETH_PACKET_FILTER;
      phost->Control.setup.b.wValue.w = ECM_PACKET_FILTER;
      phost->Control.setup.b.wIndex.w = h->CommItfNum;
      phost->Control.setup.b.wLength.w = 0U;

      req = USBH_CtlReq(phost, NULL, 0U);
      if (req == USBH_OK)
      {
        h->proto_state = (h->imac_str_idx != 0U) ? ECM_SETUP_GET_MAC : ECM_SETUP_FINISH;
      }
      else if (req != USBH_BUSY)
      {
        /* Tolerate a filter failure - many dongles default to receiving. */
        h->proto_state = (h->imac_str_idx != 0U) ? ECM_SETUP_GET_MAC : ECM_SETUP_FINISH;
      }
      return USBH_BUSY;

    case ECM_SETUP_GET_MAC:
      req = USBH_Get_StringDesc(phost, h->imac_str_idx, mac_str, sizeof(mac_str));
      if (req == USBH_OK)
      {
        if (ecm_parse_mac(mac_str, h->mac_addr) != 0U)
        {
          h->mac_valid = 1U;
        }
        h->proto_state = ECM_SETUP_FINISH;
      }
      else if (req != USBH_BUSY)
      {
        h->proto_state = ECM_SETUP_FINISH;
      }
      return USBH_BUSY;

    case ECM_SETUP_FINISH:
    default:
      if (h->mac_valid == 0U)
      {
        ecm_fallback_mac(h->mac_addr);
        h->mac_valid = 1U;
      }
      return USBH_OK;
  }
}

static void ECM_RxUnwrap(USBH_HandleTypeDef *phost, uint8_t *buf, uint32_t len)
{
  /* One Ethernet frame (without FCS) per bulk transfer. */
  USBH_RTKNIC_DeliverFrame(phost, buf, len);
}

static uint32_t ECM_TxWrap(USBH_HandleTypeDef *phost, const uint8_t *frame, uint32_t len)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;

  (void)USBH_memcpy(h->tx_buf, frame, len);
  h->tx_zlp = ((h->OutEpSize != 0U) && ((len % h->OutEpSize) == 0U)) ? 1U : 0U;
  return len;
}

const RTKNIC_ProtoOpsTypeDef RTKNIC_ECM_Ops =
{
  RTKNIC_PROTO_ECM,
  RTKNIC_CDC_SUBCLASS_ECM,
  ECM_Setup,
  ECM_RxUnwrap,
  ECM_TxWrap,
};
