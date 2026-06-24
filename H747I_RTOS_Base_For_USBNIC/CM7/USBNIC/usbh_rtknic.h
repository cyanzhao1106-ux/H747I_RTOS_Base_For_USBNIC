/**
  ******************************************************************************
  * @file    usbh_rtknic.h
  * @author  USB NIC integration
  * @brief   USB Host class driver for Realtek USB LAN chips
  *          (RTL8152B / 8153B / 8156B / 8157 / 8159).
  *
  *          These dongles enumerate with a vendor-specific configuration as
  *          configuration index 0 (bInterfaceClass = 0xFF, which is what the
  *          ST host enumerator latches onto), plus one or more CDC
  *          configurations:
  *            - 8152B / 8153B : cfg1 = Vendor, cfg2 = CDC-ECM
  *            - 8156B/8157/8159: cfg1 = Vendor, cfg2 = CDC-NCM, cfg3 = CDC-ECM
  *
  *          This driver advertises ClassCode 0xFF so it is selected for the
  *          vendor configuration, then scans the remaining configurations and
  *          issues SET_CONFIGURATION to switch the device into the CDC
  *          networking configuration (ECM or NCM, chosen at build time), opens
  *          the bulk data + interrupt notification pipes and exposes a simple
  *          Ethernet frame TX/RX API consumed by the lwIP netif glue.
  ******************************************************************************
  */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef __USBH_RTKNIC_H
#define __USBH_RTKNIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/
#include "usbh_core.h"

/* Exported constants -------------------------------------------------------*/

/* Realtek USB vendor ID - used to scope this class to Realtek dongles only   */
#define RTKNIC_VID_REALTEK              0x0BDAU

/* Product IDs (lower 16 bits match the marketing chip number for these parts) */
#define RTKNIC_PID_RTL8152              0x8152U
#define RTKNIC_PID_RTL8153              0x8153U
#define RTKNIC_PID_RTL8156              0x8156U
#define RTKNIC_PID_RTL8157              0x8157U
#define RTKNIC_PID_RTL8159              0x815AU

/* Interface class / subclass codes used to locate the CDC networking model   */
#define RTKNIC_VENDOR_CLASS             0xFFU   /* vendor cfg interface class  */
#define RTKNIC_CDC_CLASS_COMM           0x02U   /* communications interface    */
#define RTKNIC_CDC_CLASS_DATA           0x0AU   /* CDC data interface          */
#define RTKNIC_CDC_SUBCLASS_ECM         0x06U   /* Ethernet Networking Model   */
#define RTKNIC_CDC_SUBCLASS_NCM         0x0DU   /* Network Control Model       */

/* CDC functional descriptor identifiers (in the raw config descriptor)       */
#define RTKNIC_CS_INTERFACE             0x24U
#define RTKNIC_CDC_FUNC_ETHERNET        0x0FU   /* Ethernet Networking func.   */

/* CDC notifications carried on the interrupt endpoint (ECM/NCM, CDC 1.20 6.3) */
#define RTKNIC_NOTIF_REQTYPE            0xA1U   /* D2H | class | interface     */
#define RTKNIC_NOTIF_NETWORK_CONNECTION 0x00U  /* wValue: 1=connect 0=disconn */
#define RTKNIC_NOTIF_RESPONSE_AVAILABLE 0x01U
#define RTKNIC_NOTIF_CONNECTION_SPEED   0x2AU  /* + DLBitRate + ULBitRate (8B) */
#define RTKNIC_NOTIF_HDR_LEN            8U      /* CDC notification header size */

/* CDC class-specific requests                                                */
#define RTKNIC_REQ_SET_ETH_PACKET_FILTER 0x43U
#define RTKNIC_REQ_GET_NTB_PARAMETERS    0x80U
#define RTKNIC_REQ_SET_NTB_INPUT_SIZE    0x86U
#define RTKNIC_REQ_GET_NTB_INPUT_SIZE    0x85U
#define RTKNIC_REQ_SET_NTB_FORMAT        0x84U
#define RTKNIC_REQ_SET_CRC_MODE          0x8AU

/* Ethernet packet filter bits (CDC ECM 6.2.4)                                */
#define RTKNIC_PKT_FILTER_PROMISCUOUS   0x0001U
#define RTKNIC_PKT_FILTER_ALL_MULTICAST 0x0002U
#define RTKNIC_PKT_FILTER_DIRECTED      0x0004U
#define RTKNIC_PKT_FILTER_BROADCAST     0x0008U
#define RTKNIC_PKT_FILTER_MULTICAST     0x0010U

/* ------------------------------------------------------------------------- */
/* PLA / USB OCP register access (vendor EP0 control transfer)                */
/* ------------------------------------------------------------------------- */

/* Vendor request code used for both OCP register read and write.             */
#define RTKNIC_REQ_VENDOR_OCP           0x05U

/* OCP register space selector - placed in setup wIndex bit 8.                */
#define RTKNIC_MCU_TYPE_USB             0x0000U  /* USB MCU OCP registers      */
#define RTKNIC_MCU_TYPE_PLA             0x0100U  /* PLA MCU OCP registers      */

/* Byte-enable masks for the setup wIndex low byte: bits 3..0 select bytes of
   the first DWORD, bits 7..4 select bytes of the last DWORD. For a single
   aligned DWORD the two nibbles address the same DWORD, so 0xFF/0x33/0x11
   shifted to the byte offset selects the desired DWORD/word/byte.            */
#define RTKNIC_BYTE_EN_DWORD            0xFFU
#define RTKNIC_BYTE_EN_WORD             0x33U
#define RTKNIC_BYTE_EN_BYTE             0x11U

/* Maximum payload per OCP control transfer (spec: multiple of 4, <= 512).    */
#define RTKNIC_OCP_MAX_CHUNK            512U

#define RTKNIC_MAC_ADDR_LEN             6U
#define RTKNIC_ETH_MAX_FRAME            1514U
/* RX buffer: must be a multiple of the HS bulk MPS (512) and <= 65535 (the
   bulk-IN request length is passed as a u16). It also caps the NCM NTB-IN size
   requested from the device (see NCM_Setup), i.e. how many datagrams the RTL815x
   may coalesce into one block. Kept at 2048 (one full frame per NTB).
   MEASURED: larger NTB is a throughput LOSS regardless of buffering - the
   device's RX coalescing timer is driven by the device, not the host, so a big
   NTB-IN just makes it wait longer/accumulate more before flushing, inflating
   per-NTB latency (8192 gave ~5-19 Mbps; 2048 gives ~108 Mbps). Ping-pong (two
   buffers, always-armed bulk-IN; see usbh_rtknic.c) hides the host-side
   processing gap on top of this, but cannot hide device-side coalescing - hence
   small NTB + ping-pong is the sweet spot. The two RX buffers are static, out of
   the malloc'd handle, to avoid the 16 KB CM7 C-heap. */
#define RTKNIC_RX_BUF_SIZE              2048U
#define RTKNIC_TX_BUF_SIZE              2048U   /* single Ethernet frame        */
#define RTKNIC_NOTIF_BUF_SIZE           16U

/* Depth of the TX frame queue (raw Ethernet frames awaiting transmit). Sized to
   absorb a burst of TCP ACKs/window-updates without blocking the TCP/IP thread.
   Must be a power of two so (index % depth) is a cheap mask. */
#define RTKNIC_TX_QUEUE_DEPTH           8U

/* Protocol selection -------------------------------------------------------*/
typedef enum
{
  RTKNIC_PROTO_ECM = 0,
  RTKNIC_PROTO_NCM
} RTKNIC_ProtoTypeDef;

/* Bring-up (ClassRequest) state machine                                      */
typedef enum
{
  RTKNIC_REQ_GET_CFG = 0,
  RTKNIC_REQ_SET_CFG,
  RTKNIC_REQ_SET_IF,
  RTKNIC_REQ_PROTO,
  RTKNIC_REQ_DONE,
  RTKNIC_REQ_ERROR
} RTKNIC_ReqStateTypeDef;

/* Runtime data path state machines                                           */
typedef enum
{
  RTKNIC_STATE_IDLE = 0,
  RTKNIC_STATE_TRANSFER,
  RTKNIC_STATE_ERROR
} RTKNIC_StateTypeDef;

typedef enum
{
  RTKNIC_RX_SUBMIT = 0,
  RTKNIC_RX_WAIT
} RTKNIC_RxStateTypeDef;

typedef enum
{
  RTKNIC_TX_IDLE = 0,
  RTKNIC_TX_SEND,
  RTKNIC_TX_WAIT,
  RTKNIC_TX_ZLP,
  RTKNIC_TX_ZLP_WAIT
} RTKNIC_TxStateTypeDef;

typedef enum
{
  RTKNIC_NOTIF_SUBMIT = 0,  /* (re)arm the interrupt IN transfer             */
  RTKNIC_NOTIF_WAIT         /* transfer in flight; SOF timer drives the rearm */
} RTKNIC_NotifStateTypeDef;

struct RTKNIC_Handle;

/* Protocol operations -- ECM and NCM each provide one of these tables.       */
typedef struct
{
  RTKNIC_ProtoTypeDef type;
  uint8_t  cdc_subclass;   /* communications interface subclass to match     */

  /* Class-specific bring-up after SET_INTERFACE. Acts as a sub-state machine:
     returns USBH_OK when complete, USBH_BUSY while in progress.             */
  USBH_StatusTypeDef (*setup)(USBH_HandleTypeDef *phost);

  /* Parse a completed bulk-IN transfer (len bytes at buf, one of the RX
     ping-pong buffers) into one or more Ethernet frames, invoking
     USBH_RTKNIC_OnRxFrame() for each.                                       */
  void (*rx_unwrap)(USBH_HandleTypeDef *phost, uint8_t *buf, uint32_t len);

  /* Wrap one Ethernet frame into the bulk-OUT payload in h->tx_buf and
     return the number of bytes to transmit (sets h->tx_zlp when a
     terminating zero-length packet is required).                            */
  uint32_t (*tx_wrap)(USBH_HandleTypeDef *phost, const uint8_t *frame, uint32_t len);
} RTKNIC_ProtoOpsTypeDef;

/* Class handle                                                               */
typedef struct RTKNIC_Handle
{
  /* endpoints / pipes */
  uint8_t  NotifEp;
  uint8_t  NotifPipe;
  uint16_t NotifEpSize;
  uint8_t  NotifEpInterval;  /* bInterval of the interrupt notification EP    */
  uint8_t  InEp;
  uint8_t  InPipe;
  uint16_t InEpSize;
  uint8_t  OutEp;
  uint8_t  OutPipe;
  uint16_t OutEpSize;

  /* interfaces */
  uint8_t  CommItfNum;
  uint8_t  DataItfNum;
  uint8_t  DataAltSet;

  /* bring-up state */
  uint8_t  req_state;
  uint8_t  scan_idx;
  uint8_t  num_cfg;
  uint8_t  target_cfg_val;
  uint8_t  proto_state;
  uint8_t  imac_str_idx;

  /* runtime state */
  uint8_t  state;
  uint8_t  rx_state;
  uint8_t  tx_state;
  uint8_t  notif_state;
  uint8_t  notif_handled;    /* current transfer's URB result already consumed */
  uint16_t notif_poll;       /* interrupt poll period in Timer ticks (SOFs)   */
  uint32_t notif_timer;      /* phost->Timer snapshot of last submit/re-arm   */

  uint32_t rx_req_len;

  /* RX ping-pong: index (0/1) of the buffer the in-flight bulk-IN is filling.
     The buffers themselves are static in usbh_rtknic.c (kept out of this
     malloc'd handle so the 2 x RTKNIC_RX_BUF_SIZE does not blow the 16 KB
     CM7 C-heap; AXI-SRAM placement keeps them OTG-DMA reachable). */
  uint8_t  rx_idx;

  /* TX. Frames are queued by the TCP/IP thread into a lock-free SPSC ring
     (s_tx_ring in usbh_rtknic.c, holding RAW Ethernet frames) and drained
     asynchronously by the USB host thread - so an in-flight bulk-OUT never
     blocks the TCP/IP thread (which would stall ACK/window-update delivery and
     starve RX). tx_prod is written only by the producer (Transmit), tx_cons
     only by the consumer (ProcessTransmission); occupancy = tx_prod - tx_cons.
     tx_buf holds the single frame currently being wrapped + sent. */
  uint8_t  tx_buf[RTKNIC_TX_BUF_SIZE] __attribute__((aligned(4)));
  uint32_t tx_len;            /* wrapped length of the in-flight frame          */
  uint8_t  tx_zlp;            /* in-flight frame needs a terminating ZLP        */
  volatile uint32_t tx_prod;  /* producer index (TCP/IP thread)                 */
  volatile uint32_t tx_cons;  /* consumer index (USB host thread)               */
  uint16_t tx_ring_len[RTKNIC_TX_QUEUE_DEPTH];  /* raw frame length per slot    */

  /* notification. Explicitly 4-byte aligned: it follows odd-sized fields above
     so its natural offset is not word-aligned, but the interrupt-IN transfer
     it feeds also uses DMA. */
  uint8_t  notif_buf[RTKNIC_NOTIF_BUF_SIZE] __attribute__((aligned(4)));

  /* MAC / link */
  uint8_t  mac_addr[RTKNIC_MAC_ADDR_LEN];
  uint8_t  mac_valid;
  uint8_t  link_up;
  uint32_t link_speed_dl;   /* downlink bits/sec (CONNECTION_SPEED_CHANGE)    */
  uint32_t link_speed_ul;   /* uplink   bits/sec (0 = not reported yet)       */

  /* protocol */
  RTKNIC_ProtoTypeDef proto;
  const RTKNIC_ProtoOpsTypeDef *ops;

  /* protocol private scratch (NCM sequence number, NTB sizes, sub-states) */
  uint32_t priv[8];
} RTKNIC_HandleTypeDef;

/* Exported class structure (registered with the USB host stack)              */
extern USBH_ClassTypeDef  RTKNIC_Class;
#define USBH_RTKNIC_CLASS  &RTKNIC_Class

/* Protocol op tables (defined in usbh_rtknic_ecm.c / usbh_rtknic_ncm.c)      */
extern const RTKNIC_ProtoOpsTypeDef RTKNIC_ECM_Ops;
extern const RTKNIC_ProtoOpsTypeDef RTKNIC_NCM_Ops;

/* Exported API -------------------------------------------------------------*/

/* Queue one Ethernet frame for transmission (called from the netif).
   Returns USBH_OK if accepted, USBH_BUSY if a frame is still in flight.     */
USBH_StatusTypeDef USBH_RTKNIC_Transmit(USBH_HandleTypeDef *phost,
                                        const uint8_t *frame, uint32_t len);

/* Accessors used by the netif glue.                                          */
uint8_t  USBH_RTKNIC_GetMacAddr(USBH_HandleTypeDef *phost, uint8_t *mac);
uint8_t  USBH_RTKNIC_IsLinkUp(USBH_HandleTypeDef *phost);
uint16_t USBH_RTKNIC_GetMTU(USBH_HandleTypeDef *phost);

/* Last link speed reported via CDC CONNECTION_SPEED_CHANGE, in bits/sec.
   Returns 1 and fills *dl_bps / *ul_bps if a speed has been reported, else 0.
   Note: the CDC bitrate fields are 32-bit and saturate at ~4.29 Gbps, so the
   5G/10G-capable parts (8157/8159) cannot express their true rate here and may
   report 0xFFFFFFFF for rates beyond that range.                             */
uint8_t  USBH_RTKNIC_GetLinkSpeed(USBH_HandleTypeDef *phost,
                                  uint32_t *dl_bps, uint32_t *ul_bps);

/* Active transport protocol (ECM or NCM) negotiated for the attached device. */
RTKNIC_ProtoTypeDef USBH_RTKNIC_GetProto(USBH_HandleTypeDef *phost);

/* Weak callbacks implemented by the netif glue.                              */
void USBH_RTKNIC_OnRxFrame(USBH_HandleTypeDef *phost, uint8_t *frame, uint32_t len);
void USBH_RTKNIC_OnLinkChange(USBH_HandleTypeDef *phost, uint8_t link_up);
void USBH_RTKNIC_OnActive(USBH_HandleTypeDef *phost);
void USBH_RTKNIC_OnDisconnect(USBH_HandleTypeDef *phost);

/* Helper exposed to protocol modules: deliver a parsed datagram upward.      */
void USBH_RTKNIC_DeliverFrame(USBH_HandleTypeDef *phost, uint8_t *frame, uint32_t len);

/* ------------------------------------------------------------------------- */
/* PLA / USB OCP register access (usbh_rtknic_reg.c)                          */
/*                                                                            */
/* Read/write the device's PLA (Ethernet MAC/PHY) and USB MCU register spaces */
/* through vendor control transfers on EP0, used for per-platform Tx/Rx       */
/* performance tuning of the RTL8152B/8153B/8156B/8157/8159.                  */
/*                                                                            */
/* These calls BLOCK until the EP0 transfer completes, so they must run in    */
/* the USB host task context (e.g. from a protocol setup() step or the        */
/* USBH_RTKNIC_OnActive() hook) - never from an ISR, and not concurrently     */
/* with the data path from another task.                                      */
/*                                                                            */
/* 'type' is RTKNIC_MCU_TYPE_PLA or RTKNIC_MCU_TYPE_USB. Register addresses    */
/* must be 4-byte aligned (word: 2-byte aligned; byte: any). The block        */
/* helpers additionally require 'len' to be a multiple of 4. All data is      */
/* little-endian on the wire. Return USBH_OK on success, else USBH_FAIL.      */
/* ------------------------------------------------------------------------- */
USBH_StatusTypeDef USBH_RTKNIC_OcpRead(USBH_HandleTypeDef *phost, uint16_t type,
                                       uint16_t addr, void *data, uint16_t len);
USBH_StatusTypeDef USBH_RTKNIC_OcpWrite(USBH_HandleTypeDef *phost, uint16_t type,
                                        uint16_t addr, const void *data, uint16_t len);

USBH_StatusTypeDef USBH_RTKNIC_OcpReadDword(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint32_t *val);
USBH_StatusTypeDef USBH_RTKNIC_OcpWriteDword(USBH_HandleTypeDef *phost, uint16_t type,
                                             uint16_t addr, uint32_t val);
USBH_StatusTypeDef USBH_RTKNIC_OcpReadWord(USBH_HandleTypeDef *phost, uint16_t type,
                                           uint16_t addr, uint16_t *val);
USBH_StatusTypeDef USBH_RTKNIC_OcpWriteWord(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint16_t val);
USBH_StatusTypeDef USBH_RTKNIC_OcpReadByte(USBH_HandleTypeDef *phost, uint16_t type,
                                           uint16_t addr, uint8_t *val);
USBH_StatusTypeDef USBH_RTKNIC_OcpWriteByte(USBH_HandleTypeDef *phost, uint16_t type,
                                            uint16_t addr, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* __USBH_RTKNIC_H */
