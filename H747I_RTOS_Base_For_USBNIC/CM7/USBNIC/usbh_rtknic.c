/**
  ******************************************************************************
  * @file    usbh_rtknic.c
  * @author  USB NIC integration
  * @brief   USB Host class driver core for Realtek USB LAN chips.
  *          Handles configuration selection (vendor -> CDC ECM/NCM), pipe
  *          management and the Ethernet frame TX/RX data path. Protocol
  *          specifics (ECM vs NCM framing) live behind RTKNIC_ProtoOpsTypeDef.
  ******************************************************************************
  */

/* Includes -----------------------------------------------------------------*/
#include "usbh_rtknic.h"
#include "usbnic_log.h"

/* Build-time protocol selection --------------------------------------------*/
#if defined(RTKNIC_USE_NCM)
#define RTKNIC_ACTIVE_OPS   (&RTKNIC_NCM_Ops)
#else
#define RTKNIC_ACTIVE_OPS   (&RTKNIC_ECM_Ops)
#endif /* RTKNIC_USE_NCM */

/* RX ping-pong buffers -----------------------------------------------------*/
/* Two bulk-IN staging buffers so a new transfer can be armed into one while the
   just-completed one is being unwrapped/delivered - the bulk-IN pipe is never
   idle (the single throughput win over the old single-buffer loop). Static, not
   in the malloc'd handle: 2 x RTKNIC_RX_BUF_SIZE (16 KB) would overflow the
   16 KB CM7 C-heap. There is only ever one NIC, so a single instance is fine.
   32-byte aligned: well above the bulk-IN DMA word-alignment need, and a cache
   line so a future D-cache MPU region can cover them cleanly. */
static uint8_t s_rx_buf[2][RTKNIC_RX_BUF_SIZE] __attribute__((aligned(32)));

/* TX frame queue -----------------------------------------------------------*/
/* Lock-free single-producer (TCP/IP thread) / single-consumer (USB host
   thread) ring of RAW Ethernet frames. Decouples the TCP/IP thread from
   bulk-OUT latency: enqueue is non-blocking, so sending an ACK can never stall
   the thread that also processes (and window-updates) the RX flow. Static for
   the same reasons as s_rx_buf (out of the 16 KB C-heap, AXI-SRAM, DMA-safe). */
static uint8_t s_tx_ring[RTKNIC_TX_QUEUE_DEPTH][RTKNIC_ETH_MAX_FRAME]
                 __attribute__((aligned(32)));

/* Private function prototypes ----------------------------------------------*/
static USBH_StatusTypeDef USBH_RTKNIC_InterfaceInit(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef USBH_RTKNIC_InterfaceDeInit(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef USBH_RTKNIC_ClassRequest(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef USBH_RTKNIC_Process(USBH_HandleTypeDef *phost);
static USBH_StatusTypeDef USBH_RTKNIC_SOFProcess(USBH_HandleTypeDef *phost);

static uint8_t RTKNIC_ScanConfig(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);
static void    RTKNIC_OpenPipes(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);
static void    RTKNIC_ProcessReception(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);
static void    RTKNIC_ProcessTransmission(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);
static void    RTKNIC_ProcessNotification(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);
static void    RTKNIC_HandleNotification(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h, uint32_t len);
static void    RTKNIC_NotifPoll(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h);

/* Class structure registered with the USB host stack -----------------------*/
USBH_ClassTypeDef  RTKNIC_Class =
{
  "RTKNIC",
  RTKNIC_VENDOR_CLASS,          /* matches Realtek vendor configuration (0xFF) */
  USBH_RTKNIC_InterfaceInit,
  USBH_RTKNIC_InterfaceDeInit,
  USBH_RTKNIC_ClassRequest,
  USBH_RTKNIC_Process,
  USBH_RTKNIC_SOFProcess,
  NULL,
};

/* Helpers ------------------------------------------------------------------*/
static uint16_t rtknic_le16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rtknic_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
  * @brief  Walk the raw configuration descriptor currently held in
  *         phost->device.CfgDesc_Raw and, if it carries a CDC networking model
  *         matching the active protocol's subclass, record the communication
  *         interface, the data interface alternate setting that exposes the
  *         bulk endpoints, the bulk IN/OUT and interrupt notification
  *         endpoints, and the iMACAddress string index.
  * @retval 1 if a matching configuration was found, 0 otherwise.
  */
static uint8_t RTKNIC_ScanConfig(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  uint8_t  *buf = phost->device.CfgDesc_Raw;
  uint16_t  total = phost->device.CfgDesc.wTotalLength;
  uint16_t  ptr;
  uint8_t   comm_found = 0U;
  uint8_t   data_found = 0U;
  uint8_t   in_comm = 0U;
  uint8_t   in_data_alt = 0U;

  if ((total < 4U) || (total > sizeof(phost->device.CfgDesc_Raw)))
  {
    return 0U;
  }

  /* Start just past the configuration header (9 bytes). */
  ptr = 9U;
  while ((ptr + 2U) <= total)
  {
    uint8_t blen = buf[ptr];
    uint8_t btype = buf[ptr + 1U];

    if (blen < 2U)
    {
      break;                      /* malformed - avoid an infinite loop      */
    }
    if ((ptr + blen) > total)
    {
      break;
    }

    if (btype == 0x04U)           /* INTERFACE descriptor                     */
    {
      uint8_t if_num   = buf[ptr + 2U];
      uint8_t alt      = buf[ptr + 3U];
      uint8_t num_ep   = buf[ptr + 4U];
      uint8_t if_class = buf[ptr + 5U];
      uint8_t if_sub   = buf[ptr + 6U];

      in_comm = 0U;
      in_data_alt = 0U;

      if ((if_class == RTKNIC_CDC_CLASS_COMM) &&
          (if_sub == h->ops->cdc_subclass))
      {
        comm_found = 1U;
        in_comm = 1U;
        h->CommItfNum = if_num;
      }
      else if ((if_class == RTKNIC_CDC_CLASS_DATA) && (num_ep >= 2U) &&
               (data_found == 0U))
      {
        /* The alternate setting that actually carries the bulk endpoints. */
        data_found = 1U;
        in_data_alt = 1U;
        h->DataItfNum = if_num;
        h->DataAltSet = alt;
        h->InEp = 0U;
        h->OutEp = 0U;
      }
    }
    else if (btype == 0x05U)      /* ENDPOINT descriptor                      */
    {
      uint8_t  ep_addr = buf[ptr + 2U];
      uint8_t  ep_attr = (uint8_t)(buf[ptr + 3U] & 0x03U);
      uint16_t ep_mps  = rtknic_le16(&buf[ptr + 4U]);

      if ((in_comm != 0U) && (ep_attr == 0x03U) && ((ep_addr & 0x80U) != 0U))
      {
        h->NotifEp = ep_addr;
        h->NotifEpSize = ep_mps;
        h->NotifEpInterval = buf[ptr + 6U];   /* bInterval                     */
      }
      else if ((in_data_alt != 0U) && (ep_attr == 0x02U))
      {
        if ((ep_addr & 0x80U) != 0U)
        {
          h->InEp = ep_addr;
          h->InEpSize = ep_mps;
        }
        else
        {
          h->OutEp = ep_addr;
          h->OutEpSize = ep_mps;
        }
      }
    }
    else if (btype == RTKNIC_CS_INTERFACE)   /* CDC functional descriptor    */
    {
      if ((blen >= 4U) && (buf[ptr + 2U] == RTKNIC_CDC_FUNC_ETHERNET))
      {
        h->imac_str_idx = buf[ptr + 3U];     /* iMACAddress string index     */
      }
    }

    ptr += blen;
  }

  return (uint8_t)((comm_found != 0U) && (data_found != 0U) &&
                   (h->InEp != 0U) && (h->OutEp != 0U));
}

/**
  * @brief  Allocate and open the interrupt notification, bulk IN and bulk OUT
  *         pipes for the selected CDC networking configuration.
  */
static void RTKNIC_OpenPipes(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  if (h->NotifEp != 0U)
  {
    h->NotifPipe = USBH_AllocPipe(phost, h->NotifEp);
    (void)USBH_OpenPipe(phost, h->NotifPipe, h->NotifEp, phost->device.address,
                        phost->device.speed, USB_EP_TYPE_INTR, h->NotifEpSize);
    (void)USBH_LL_SetToggle(phost, h->NotifPipe, 0U);
  }

  h->OutPipe = USBH_AllocPipe(phost, h->OutEp);
  (void)USBH_OpenPipe(phost, h->OutPipe, h->OutEp, phost->device.address,
                      phost->device.speed, USB_EP_TYPE_BULK, h->OutEpSize);
  (void)USBH_LL_SetToggle(phost, h->OutPipe, 0U);

  h->InPipe = USBH_AllocPipe(phost, h->InEp);
  (void)USBH_OpenPipe(phost, h->InPipe, h->InEp, phost->device.address,
                      phost->device.speed, USB_EP_TYPE_BULK, h->InEpSize);
  (void)USBH_LL_SetToggle(phost, h->InPipe, 0U);

  /* Request length must be a multiple of the bulk max packet size. */
  if (h->InEpSize != 0U)
  {
    h->rx_req_len = (RTKNIC_RX_BUF_SIZE / h->InEpSize) * h->InEpSize;
  }
  else
  {
    h->rx_req_len = RTKNIC_RX_BUF_SIZE;
  }
}

/* Class driver entry points ------------------------------------------------*/

static USBH_StatusTypeDef USBH_RTKNIC_InterfaceInit(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h;

  /* Scope this class to Realtek dongles only so we do not hijack other
     vendor-class devices that happen to enumerate first. */
  if (phost->device.DevDesc.idVendor != RTKNIC_VID_REALTEK)
  {
    return USBH_FAIL;
  }

#if defined(RTKNIC_USE_NCM)
  /* NCM is only offered by 8156B / 8157 / 8159. */
  {
    uint16_t pid = phost->device.DevDesc.idProduct;
    if ((pid != RTKNIC_PID_RTL8156) && (pid != RTKNIC_PID_RTL8157) &&
        (pid != RTKNIC_PID_RTL8159))
    {
      return USBH_FAIL;
    }
  }
#endif /* RTKNIC_USE_NCM */

  phost->pActiveClass->pData = (RTKNIC_HandleTypeDef *)USBH_malloc(sizeof(RTKNIC_HandleTypeDef));
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  if (h == NULL)
  {
    USBH_ErrLog("RTKNIC: cannot allocate class handle");
    return USBH_FAIL;
  }
  (void)USBH_memset(h, 0, sizeof(RTKNIC_HandleTypeDef));

  h->ops = RTKNIC_ACTIVE_OPS;
  h->proto = h->ops->type;
  h->req_state = RTKNIC_REQ_GET_CFG;
  h->scan_idx = 0U;
  h->num_cfg = phost->device.DevDesc.bNumConfigurations;
  if (h->num_cfg == 0U)
  {
    h->num_cfg = 1U;
  }

  LOG("RTKNIC: InterfaceInit VID=%04X PID=%04X proto=%s numcfg=%u",
      (unsigned)phost->device.DevDesc.idVendor,
      (unsigned)phost->device.DevDesc.idProduct,
      (h->ops->type == RTKNIC_PROTO_NCM) ? "NCM" : "ECM", (unsigned)h->num_cfg);

  return USBH_OK;
}

static USBH_StatusTypeDef USBH_RTKNIC_InterfaceDeInit(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;

  if (h != NULL)
  {
    if (h->NotifPipe != 0U)
    {
      (void)USBH_ClosePipe(phost, h->NotifPipe);
      (void)USBH_FreePipe(phost, h->NotifPipe);
      h->NotifPipe = 0U;
    }
    if (h->InPipe != 0U)
    {
      (void)USBH_ClosePipe(phost, h->InPipe);
      (void)USBH_FreePipe(phost, h->InPipe);
      h->InPipe = 0U;
    }
    if (h->OutPipe != 0U)
    {
      (void)USBH_ClosePipe(phost, h->OutPipe);
      (void)USBH_FreePipe(phost, h->OutPipe);
      h->OutPipe = 0U;
    }
  }

  USBH_RTKNIC_OnDisconnect(phost);

  if (phost->pActiveClass->pData != NULL)
  {
    USBH_free(phost->pActiveClass->pData);
    phost->pActiveClass->pData = NULL;
  }

  return USBH_OK;
}

static USBH_StatusTypeDef USBH_RTKNIC_ClassRequest(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  USBH_StatusTypeDef status = USBH_BUSY;
  USBH_StatusTypeDef req;

  switch (h->req_state)
  {
    case RTKNIC_REQ_GET_CFG:
      req = USBH_Get_CfgDesc_Idx(phost, h->scan_idx, USBH_MAX_SIZE_CONFIGURATION);
      if (req == USBH_OK)
      {
        if (RTKNIC_ScanConfig(phost, h) != 0U)
        {
          h->target_cfg_val = phost->device.CfgDesc.bConfigurationValue;
          LOG("RTKNIC: matched cfg idx=%u val=%u comm=%u data=%u alt=%u InEp=0x%02X OutEp=0x%02X NotifEp=0x%02X imac=%u",
              (unsigned)h->scan_idx, (unsigned)h->target_cfg_val,
              (unsigned)h->CommItfNum, (unsigned)h->DataItfNum, (unsigned)h->DataAltSet,
              (unsigned)h->InEp, (unsigned)h->OutEp, (unsigned)h->NotifEp,
              (unsigned)h->imac_str_idx);
          h->req_state = RTKNIC_REQ_SET_CFG;
        }
        else
        {
          h->scan_idx++;
          if (h->scan_idx >= h->num_cfg)
          {
            LOG("RTKNIC: no matching CDC networking configuration (%u cfgs)",
                (unsigned)h->num_cfg);
            h->req_state = RTKNIC_REQ_ERROR;
          }
        }
      }
      else if (req != USBH_BUSY)
      {
        LOG("RTKNIC: GET_CFG idx=%u failed st=%d", (unsigned)h->scan_idx, (int)req);
        h->req_state = RTKNIC_REQ_ERROR;
      }
      break;

    case RTKNIC_REQ_SET_CFG:
      req = USBH_SetCfg(phost, h->target_cfg_val);
      if (req == USBH_OK)
      {
        RTKNIC_OpenPipes(phost, h);
        h->req_state = RTKNIC_REQ_SET_IF;
      }
      else if (req != USBH_BUSY)
      {
        LOG("RTKNIC: SET_CFG(%u) failed st=%d", (unsigned)h->target_cfg_val, (int)req);
        h->req_state = RTKNIC_REQ_ERROR;
      }
      break;

    case RTKNIC_REQ_SET_IF:
      if (h->DataAltSet == 0U)
      {
        /* No alternate setting required (bulk endpoints on alt 0). */
        h->req_state = RTKNIC_REQ_PROTO;
        break;
      }
      req = USBH_SetInterface(phost, h->DataItfNum, h->DataAltSet);
      if (req == USBH_OK)
      {
        h->req_state = RTKNIC_REQ_PROTO;
      }
      else if (req != USBH_BUSY)
      {
        LOG("RTKNIC: SET_IF(itf=%u,alt=%u) failed st=%d",
            (unsigned)h->DataItfNum, (unsigned)h->DataAltSet, (int)req);
        h->req_state = RTKNIC_REQ_ERROR;
      }
      break;

    case RTKNIC_REQ_PROTO:
      req = h->ops->setup(phost);
      if ((req == USBH_OK) || (req == USBH_NOT_SUPPORTED))
      {
        h->req_state = RTKNIC_REQ_DONE;
      }
      else if (req != USBH_BUSY)
      {
        LOG("RTKNIC: proto setup failed st=%d", (int)req);
        h->req_state = RTKNIC_REQ_ERROR;
      }
      break;

    case RTKNIC_REQ_DONE:
      h->state = RTKNIC_STATE_TRANSFER;
      h->rx_state = RTKNIC_RX_SUBMIT;
      h->tx_state = RTKNIC_TX_IDLE;
      h->notif_state = RTKNIC_NOTIF_SUBMIT;
      h->notif_handled = 0U;
      h->notif_timer = phost->Timer;
      /* Poll period for the interrupt notification EP, expressed in phost->Timer
         ticks. phost->Timer is incremented once per (micro)SOF: 125us/tick on a
         HS port, 1ms/tick on FS/LS. The bInterval field is encoded to match:
         HS = 2^(bInterval-1) microframes, FS/LS = interval in ms. So in both
         cases the period in Timer ticks is exactly those raw units - do NOT
         convert to ms (that was an 8x over-poll bug on HS). */
      {
        uint8_t bi = h->NotifEpInterval;

        if (phost->device.speed == (uint8_t)USBH_SPEED_HIGH)
        {
          if (bi == 0U)  { bi = 1U; }
          if (bi > 16U)  { bi = 16U; }
          h->notif_poll = (uint16_t)(1UL << (bi - 1U));   /* microframes = ticks */
        }
        else
        {
          h->notif_poll = (uint16_t)bi;                   /* ms = ticks          */
        }
        if (h->notif_poll == 0U) { h->notif_poll = 8U; }  /* sane floor          */
      }
      /* Link starts DOWN/unknown and is driven solely by the device's
         NETWORK_CONNECTION notification on the interrupt endpoint. Do not
         assert "up" here - that would report a link even with no cable. */
      h->link_up = 0U;
      h->link_speed_dl = 0U;
      h->link_speed_ul = 0U;
      LOG("RTKNIC: active proto=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X (valid=%u)",
          (h->ops->type == RTKNIC_PROTO_NCM) ? "NCM" : "ECM",
          h->mac_addr[0], h->mac_addr[1], h->mac_addr[2],
          h->mac_addr[3], h->mac_addr[4], h->mac_addr[5], (unsigned)h->mac_valid);
      LOG("RTKNIC: InEpSize=%u OutEpSize=%u NotifEp=0x%02X size=%u poll=%uticks(~%ums)",
          (unsigned)h->InEpSize, (unsigned)h->OutEpSize,
          (unsigned)h->NotifEp, (unsigned)h->NotifEpSize, (unsigned)h->notif_poll,
          (phost->device.speed == (uint8_t)USBH_SPEED_HIGH) ?
              (unsigned)(h->notif_poll / 8U) : (unsigned)h->notif_poll);
      phost->pUser(phost, HOST_USER_CLASS_ACTIVE);
      USBH_RTKNIC_OnActive(phost);
      status = USBH_OK;
      break;

    case RTKNIC_REQ_ERROR:
    default:
      status = USBH_FAIL;
      break;
  }

  return status;
}

static USBH_StatusTypeDef USBH_RTKNIC_Process(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;

  if ((h == NULL) || (h->state != RTKNIC_STATE_TRANSFER))
  {
    return USBH_OK;
  }

  RTKNIC_ProcessReception(phost, h);
  RTKNIC_ProcessTransmission(phost, h);
  RTKNIC_ProcessNotification(phost, h);

  return USBH_OK;
}

static USBH_StatusTypeDef USBH_RTKNIC_SOFProcess(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;

  if ((h != NULL) && (h->state == RTKNIC_STATE_TRANSFER))
  {
    RTKNIC_NotifPoll(phost, h);
  }

  return USBH_OK;
}

/* Data path ----------------------------------------------------------------*/

static void RTKNIC_ProcessReception(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  USBH_URBStateTypeDef urb;
  uint32_t len;

  switch (h->rx_state)
  {
    case RTKNIC_RX_SUBMIT:
      /* Initial arm (and re-arm after a stall). Always start on buffer 0. */
      h->rx_idx = 0U;
      (void)USBH_BulkReceiveData(phost, s_rx_buf[0], (uint16_t)h->rx_req_len, h->InPipe);
      h->rx_state = RTKNIC_RX_WAIT;
      break;

    case RTKNIC_RX_WAIT:
      urb = USBH_LL_GetURBState(phost, h->InPipe);
      if (urb == USBH_URB_DONE)
      {
        uint8_t done_idx = h->rx_idx;

        len = USBH_LL_GetLastXferSize(phost, h->InPipe);

        /* Ping-pong: flip to the other buffer and re-arm the next bulk-IN
           BEFORE unwrapping, so the device keeps streaming into the fresh
           buffer while we deliver the frames from the one just completed. The
           two buffers are disjoint, so the in-flight DMA never races the
           unwrap. Stay in RX_WAIT - a transfer is now pending again. */
        h->rx_idx ^= 1U;
        (void)USBH_BulkReceiveData(phost, s_rx_buf[h->rx_idx],
                                   (uint16_t)h->rx_req_len, h->InPipe);

        if (len > 0U)
        {
          h->ops->rx_unwrap(phost, s_rx_buf[done_idx], len);
        }
#if (USBH_USE_OS == 1U)
        USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      else if (urb == USBH_URB_STALL)
      {
        (void)USBH_ClrFeature(phost, h->InEp);
        h->rx_state = RTKNIC_RX_SUBMIT;
      }
      else
      {
        /* NAK / idle: no frame waiting, keep the IN transfer pending. */
      }
      break;

    default:
      h->rx_state = RTKNIC_RX_SUBMIT;
      break;
  }
}

/* Free the TX ring slot just transmitted. Consumer side only (USB host thread):
   the wrap at TX_IDLE already copied the frame out of the slot, so publishing
   the advanced tx_cons safely hands the slot back to the producer. */
static void RTKNIC_TxConsume(RTKNIC_HandleTypeDef *h)
{
  __DMB();                 /* slot fully read before it is marked free          */
  h->tx_cons++;
}

static void RTKNIC_ProcessTransmission(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  USBH_URBStateTypeDef urb;

  switch (h->tx_state)
  {
    case RTKNIC_TX_IDLE:
      if (h->tx_prod != h->tx_cons)            /* ring non-empty: start next     */
      {
        uint32_t slot = h->tx_cons & (RTKNIC_TX_QUEUE_DEPTH - 1U);
        /* Wrap the queued raw frame into tx_buf now, on the USB host thread
           (owner of tx_buf / tx_zlp / proto seq). Sets h->tx_zlp. */
        h->tx_len = h->ops->tx_wrap(phost, s_tx_ring[slot],
                                    (uint32_t)h->tx_ring_len[slot]);
        h->tx_state = RTKNIC_TX_SEND;
      }
      else
      {
        break;
      }
      /* fall through */

    case RTKNIC_TX_SEND:
      (void)USBH_BulkSendData(phost, h->tx_buf, (uint16_t)h->tx_len, h->OutPipe, 1U);
      h->tx_state = RTKNIC_TX_WAIT;
      break;

    case RTKNIC_TX_WAIT:
      urb = USBH_LL_GetURBState(phost, h->OutPipe);
      if (urb == USBH_URB_DONE)
      {
        if (h->tx_zlp != 0U)
        {
          h->tx_zlp = 0U;
          h->tx_state = RTKNIC_TX_ZLP;
        }
        else
        {
          RTKNIC_TxConsume(h);               /* frame done: free the ring slot */
          h->tx_state = RTKNIC_TX_IDLE;
        }
#if (USBH_USE_OS == 1U)
        USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      else if (urb == USBH_URB_NOTREADY)
      {
        h->tx_state = RTKNIC_TX_SEND;       /* NAK: retransmit                */
#if (USBH_USE_OS == 1U)
        USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      else if (urb == USBH_URB_STALL)
      {
        (void)USBH_ClrFeature(phost, h->OutEp);
        RTKNIC_TxConsume(h);                 /* drop frame, free the slot      */
        h->tx_state = RTKNIC_TX_IDLE;
      }
      else
      {
        /* still in flight */
      }
      break;

    case RTKNIC_TX_ZLP:
      (void)USBH_BulkSendData(phost, h->tx_buf, 0U, h->OutPipe, 1U);
      h->tx_state = RTKNIC_TX_ZLP_WAIT;
      break;

    case RTKNIC_TX_ZLP_WAIT:
      urb = USBH_LL_GetURBState(phost, h->OutPipe);
      if (urb == USBH_URB_DONE)
      {
        RTKNIC_TxConsume(h);                 /* frame (+ZLP) done: free slot   */
        h->tx_state = RTKNIC_TX_IDLE;
#if (USBH_USE_OS == 1U)
        USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      else if (urb == USBH_URB_NOTREADY)
      {
        h->tx_state = RTKNIC_TX_ZLP;
      }
      break;

    default:
      h->tx_state = RTKNIC_TX_IDLE;
      break;
  }
}

/**
  * @brief  Decode one completed CDC notification (len bytes in h->notif_buf).
  *         Drives the link state from NETWORK_CONNECTION and records the link
  *         speed from CONNECTION_SPEED_CHANGE.
  */
static void RTKNIC_HandleNotification(USBH_HandleTypeDef *phost,
                                      RTKNIC_HandleTypeDef *h, uint32_t len)
{
  /* CDC notifications share an 8-byte header (bmRequestType, bNotificationCode,
     wValue, wIndex, wLength) optionally followed by a data payload. */
  if ((len < RTKNIC_NOTIF_HDR_LEN) ||
      (h->notif_buf[0] != RTKNIC_NOTIF_REQTYPE))
  {
    return;
  }

  switch (h->notif_buf[1])
  {
    case RTKNIC_NOTIF_NETWORK_CONNECTION:
    {
      /* wValue (bytes 2..3): 1 = connected, 0 = disconnected. */
      uint8_t up = (rtknic_le16(&h->notif_buf[2]) != 0U) ? 1U : 0U;

      if (up != h->link_up)
      {
        /* Genuine state transition - announce it prominently on the VCP log. */
        h->link_up = up;
        if (up != 0U)
        {
          LOG("RTKNIC: >>> LINK UP <<<");
        }
        else
        {
          h->link_speed_dl = 0U;     /* speed is meaningless while link down  */
          h->link_speed_ul = 0U;
          LOG("RTKNIC: >>> LINK DOWN <<<");
        }
        USBH_RTKNIC_OnLinkChange(phost, up);
      }
      /* else: device re-announced the state it is already in (this Realtek part
         level-reports the current state on every poll). Silently ignore - the
         heartbeat counters show the poll is alive without flooding the log. */
      break;
    }

    case RTKNIC_NOTIF_CONNECTION_SPEED:
      /* 8-byte header + DLBitRate (u32 LE) + ULBitRate (u32 LE). */
      if (len >= (RTKNIC_NOTIF_HDR_LEN + 8U))
      {
        uint32_t dl = rtknic_le32(&h->notif_buf[RTKNIC_NOTIF_HDR_LEN]);
        uint32_t ul = rtknic_le32(&h->notif_buf[RTKNIC_NOTIF_HDR_LEN + 4U]);

        /* 8156B level-reports the speed on every poll; 8157/8159 send it once on
           change. Only log when the rate actually changes (link-down clears these
           to 0, so a fresh link-up always re-announces). */
        if ((dl != h->link_speed_dl) || (ul != h->link_speed_ul))
        {
          h->link_speed_dl = dl;
          h->link_speed_ul = ul;

          /* The 32-bit CDC bitrate fields top out at ~4.29 Gbps. 8157 (5G) /
             8159 (5G/10G) exceed that and report a saturated 0xFFFFFFFF. */
          if ((dl == 0xFFFFFFFFU) || (ul == 0xFFFFFFFFU))
          {
            LOG("RTKNIC: CONNECTION_SPEED >4.29Gbps (CDC field saturated): DL=0x%08lX UL=0x%08lX",
                (unsigned long)dl, (unsigned long)ul);
          }
          else
          {
            LOG("RTKNIC: CONNECTION_SPEED DL=%lu Mbps UL=%lu Mbps",
                (unsigned long)(dl / 1000000UL),
                (unsigned long)(ul / 1000000UL));
          }
        }
      }
      break;

    default:
      /* RESPONSE_AVAILABLE and any vendor notifications are ignored here. */
      break;
  }
}

static void RTKNIC_ProcessNotification(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  USBH_URBStateTypeDef urb;
  uint8_t length;

  if (h->NotifPipe == 0U)
  {
    return;
  }

  /* This mirrors the ST HID class interrupt-IN poll (usbh_hid.c): the WAIT state
     only consumes a completed transfer (URB_DONE) or recovers a stall; it NEVER
     re-arms itself and never waits on a NAK. Re-arming is driven purely by the
     SOF timer in RTKNIC_NotifPoll(). That decoupling is what makes it robust: if
     a NAK halts the channel, or the channel hangs at URB_IDLE, or the OS wake is
     dropped, the next poll tick unconditionally re-submits and re-activates the
     channel - there is no state in which we can wedge forever. */
  switch (h->notif_state)
  {
    case RTKNIC_NOTIF_SUBMIT:
      length = (h->NotifEpSize < RTKNIC_NOTIF_BUF_SIZE) ?
               (uint8_t)h->NotifEpSize : (uint8_t)RTKNIC_NOTIF_BUF_SIZE;
      (void)USBH_InterruptReceiveData(phost, h->notif_buf, length, h->NotifPipe);
      h->notif_handled = 0U;
      h->notif_timer = phost->Timer;
      h->notif_state = RTKNIC_NOTIF_WAIT;
      break;

    case RTKNIC_NOTIF_WAIT:
      urb = USBH_LL_GetURBState(phost, h->NotifPipe);
      if (urb == USBH_URB_DONE)
      {
        /* Consume the notification exactly once; stay in WAIT until the SOF
           timer re-arms (DONE persists until the channel is re-submitted). */
        if (h->notif_handled == 0U)
        {
          h->notif_handled = 1U;
          RTKNIC_HandleNotification(phost, h,
                                    USBH_LL_GetLastXferSize(phost, h->NotifPipe));
        }
      }
      else if (urb == USBH_URB_STALL)
      {
        (void)USBH_ClrFeature(phost, h->NotifEp);
        h->notif_state = RTKNIC_NOTIF_SUBMIT;   /* re-arm immediately on stall   */
      }
      else
      {
        /* URB_NOTREADY/ERROR (device NAK), or URB_IDLE (still in flight):
           nothing to do - the SOF timer re-arms on the next poll period. */
      }
      break;

    default:
      h->notif_state = RTKNIC_NOTIF_SUBMIT;
      break;
  }
}

/**
  * @brief  Called from SOFProcess (per (micro)SOF). Re-arms the interrupt
  *         notification poll once the endpoint's poll period has elapsed,
  *         mirroring the ST HID class. Runs in SOF/IRQ context, so it only
  *         flips state + wakes the background process; the transfer itself is
  *         submitted there.
  *
  *         The re-arm is UNCONDITIONAL on the timer - it does not require the
  *         previous URB to have completed. This is deliberate: a halted/hung
  *         interrupt channel (URB stuck at URB_IDLE) or a dropped OS wake would
  *         otherwise wedge the poll forever (observed as the "sub" counter
  *         freezing). Re-submitting re-activates the channel regardless.
  */
static void RTKNIC_NotifPoll(USBH_HandleTypeDef *phost, RTKNIC_HandleTypeDef *h)
{
  if (h->NotifPipe == 0U)
  {
    return;
  }

  switch (h->notif_state)
  {
    case RTKNIC_NOTIF_WAIT:
      /* Poll period elapsed: re-arm the next interrupt IN transfer. */
      if ((phost->Timer - h->notif_timer) >= (uint32_t)h->notif_poll)
      {
        h->notif_state = RTKNIC_NOTIF_SUBMIT;
        h->notif_timer = phost->Timer;   /* also arms the SUBMIT safety below   */
#if (USBH_USE_OS == 1U)
        (void)USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      break;

    case RTKNIC_NOTIF_SUBMIT:
      /* The submit runs in the background thread, woken by the message above.
         If that wake was dropped (USBH_OS_PutMessage discards on a full queue)
         and nothing else wakes the thread - which can happen when the link is
         down and the bus is otherwise idle - re-post so we never wedge here. */
      if ((phost->Timer - h->notif_timer) >= (uint32_t)h->notif_poll)
      {
        h->notif_timer = phost->Timer;
#if (USBH_USE_OS == 1U)
        (void)USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif
      }
      break;

    default:
      break;
  }
}

/* Public API ---------------------------------------------------------------*/

USBH_StatusTypeDef USBH_RTKNIC_Transmit(USBH_HandleTypeDef *phost,
                                        const uint8_t *frame, uint32_t len)
{
  RTKNIC_HandleTypeDef *h;

  if ((phost == NULL) || (phost->pActiveClass != &RTKNIC_Class))
  {
    return USBH_FAIL;
  }
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  if ((h == NULL) || (h->state != RTKNIC_STATE_TRANSFER))
  {
    return USBH_FAIL;
  }
  if ((len == 0U) || (len > RTKNIC_ETH_MAX_FRAME))
  {
    return USBH_FAIL;
  }

  /* Producer side (TCP/IP thread): enqueue the RAW frame and return at once.
     Never block here - blocking would stall ACK/window-update delivery and
     throttle the RX flow. The USB host thread wraps + sends it asynchronously.
     If the ring is full the caller drops the frame (TCP will retransmit; a
     superseded pure-ACK is harmless). */
  if ((h->tx_prod - h->tx_cons) >= RTKNIC_TX_QUEUE_DEPTH)
  {
    return USBH_BUSY;
  }

  {
    uint32_t slot = h->tx_prod & (RTKNIC_TX_QUEUE_DEPTH - 1U);
    (void)USBH_memcpy(s_tx_ring[slot], frame, len);
    h->tx_ring_len[slot] = (uint16_t)len;
    __DMB();                 /* frame written before the slot is published      */
    h->tx_prod++;
  }

#if (USBH_USE_OS == 1U)
  USBH_OS_PutMessage(phost, USBH_CLASS_EVENT, 0U, 0U);
#endif

  return USBH_OK;
}

void USBH_RTKNIC_DeliverFrame(USBH_HandleTypeDef *phost, uint8_t *frame, uint32_t len)
{
  if ((len >= 14U) && (len <= RTKNIC_ETH_MAX_FRAME))
  {
    USBH_RTKNIC_OnRxFrame(phost, frame, len);
  }
}

uint8_t USBH_RTKNIC_GetMacAddr(USBH_HandleTypeDef *phost, uint8_t *mac)
{
  RTKNIC_HandleTypeDef *h;

  if ((phost == NULL) || (phost->pActiveClass != &RTKNIC_Class))
  {
    return 0U;
  }
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  if ((h == NULL) || (h->mac_valid == 0U))
  {
    return 0U;
  }
  (void)USBH_memcpy(mac, h->mac_addr, RTKNIC_MAC_ADDR_LEN);
  return 1U;
}

uint8_t USBH_RTKNIC_IsLinkUp(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h;

  if ((phost == NULL) || (phost->pActiveClass != &RTKNIC_Class))
  {
    return 0U;
  }
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  return ((h != NULL) && (h->link_up != 0U)) ? 1U : 0U;
}

uint8_t USBH_RTKNIC_GetLinkSpeed(USBH_HandleTypeDef *phost,
                                 uint32_t *dl_bps, uint32_t *ul_bps)
{
  RTKNIC_HandleTypeDef *h;

  if ((phost == NULL) || (phost->pActiveClass != &RTKNIC_Class))
  {
    return 0U;
  }
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  if ((h == NULL) || ((h->link_speed_dl == 0U) && (h->link_speed_ul == 0U)))
  {
    return 0U;
  }
  if (dl_bps != NULL)
  {
    *dl_bps = h->link_speed_dl;
  }
  if (ul_bps != NULL)
  {
    *ul_bps = h->link_speed_ul;
  }
  return 1U;
}

uint16_t USBH_RTKNIC_GetMTU(USBH_HandleTypeDef *phost)
{
  UNUSED(phost);
  return 1500U;
}

RTKNIC_ProtoTypeDef USBH_RTKNIC_GetProto(USBH_HandleTypeDef *phost)
{
  RTKNIC_HandleTypeDef *h;

  if ((phost == NULL) || (phost->pActiveClass != &RTKNIC_Class))
  {
    return RTKNIC_PROTO_ECM;
  }
  h = (RTKNIC_HandleTypeDef *)phost->pActiveClass->pData;
  return (h != NULL) ? h->proto : RTKNIC_PROTO_ECM;
}

/* Default (weak) upcalls - overridden by the netif glue --------------------*/
__weak void USBH_RTKNIC_OnRxFrame(USBH_HandleTypeDef *phost, uint8_t *frame, uint32_t len)
{
  UNUSED(phost);
  UNUSED(frame);
  UNUSED(len);
}

__weak void USBH_RTKNIC_OnLinkChange(USBH_HandleTypeDef *phost, uint8_t link_up)
{
  UNUSED(phost);
  UNUSED(link_up);
}

__weak void USBH_RTKNIC_OnActive(USBH_HandleTypeDef *phost)
{
  UNUSED(phost);
}

__weak void USBH_RTKNIC_OnDisconnect(USBH_HandleTypeDef *phost)
{
  UNUSED(phost);
}
