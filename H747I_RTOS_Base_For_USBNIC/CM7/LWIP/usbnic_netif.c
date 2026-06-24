/**
  ******************************************************************************
  * @file    usbnic_netif.c
  * @author  USB NIC integration
  * @brief   lwIP network interface glue for the Realtek USB NIC host class.
  *
  *          Bridges the bare-Ethernet TX/RX API exposed by usbh_rtknic.c to a
  *          single lwIP netif. The USB host class runs in the USB host task;
  *          control-plane operations that touch lwIP state (netif add/remove,
  *          link state) are marshalled onto the TCP/IP thread via
  *          tcpip_callback(), while received frames are injected with the
  *          thread-safe tcpip_input().
  ******************************************************************************
  */

/* Includes -----------------------------------------------------------------*/
#include "usbnic_netif.h"
#include "usbh_rtknic.h"

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/lwiperf.h"

#include "cmsis_os.h"
#include "usbnic_log.h"

#include <string.h>

/* Private variables --------------------------------------------------------*/
static struct netif         s_netif;
static USBH_HandleTypeDef  *s_phost;
static volatile uint8_t     s_netif_added;

/* iperf (TCP server, port 5001). Bound to IP_ADDR_ANY, so it is independent of
   the netif address and survives DHCP renewals; kept non-NULL once started so a
   reconnect does not bind a second listener on the same port. */
static void                *s_iperf_session;

/* TX staging buffer - only ever touched from the TCP/IP thread. */
static uint8_t              s_txbuf[RTKNIC_ETH_MAX_FRAME];

/* Forward declarations -----------------------------------------------------*/
static err_t usbnic_netif_init_cb(struct netif *netif);
static err_t usbnic_linkoutput(struct netif *netif, struct pbuf *p);
static void  usbnic_status_cb(struct netif *netif);

/* netif status callback: log address changes (e.g. DHCP-assigned IP). */
static void usbnic_status_cb(struct netif *netif)
{
  if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)))
  {
    char ip[16], gw[16], mask[16];
    (void)ip4addr_ntoa_r(netif_ip4_addr(netif),    ip,   sizeof(ip));
    (void)ip4addr_ntoa_r(netif_ip4_gw(netif),      gw,   sizeof(gw));
    (void)ip4addr_ntoa_r(netif_ip4_netmask(netif), mask, sizeof(mask));
    LOG("NET: IP=%s GW=%s MASK=%s", ip, gw, mask);
  }
  else
  {
    LOG("NET: interface %s (no IP)", netif_is_up(netif) ? "up" : "down");
  }
}

/* iperf -----------------------------------------------------------------------
   Report callback, invoked on the TCP/IP thread when a session ends. */
static void usbnic_iperf_report(void *arg, enum lwiperf_report_type report_type,
                                const ip_addr_t *local_addr, u16_t local_port,
                                const ip_addr_t *remote_addr, u16_t remote_port,
                                u32_t bytes_transferred, u32_t ms_duration,
                                u32_t bandwidth_kbitpsec)
{
  char rip[16];

  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(local_addr);
  LWIP_UNUSED_ARG(local_port);

  (void)ip4addr_ntoa_r(ip_2_ip4(remote_addr), rip, sizeof(rip));

  if ((report_type == LWIPERF_TCP_DONE_SERVER) ||
      (report_type == LWIPERF_TCP_DONE_CLIENT))
  {
    LOG("IPERF: done peer=%s:%u bytes=%lu ms=%lu rate=%lu kbit/s",
        rip, (unsigned)remote_port, (unsigned long)bytes_transferred,
        (unsigned long)ms_duration, (unsigned long)bandwidth_kbitpsec);
  }
  else
  {
    LOG("IPERF: session aborted peer=%s:%u type=%d",
        rip, (unsigned)remote_port, (int)report_type);
  }
}

/* Start the TCP server once; must run on the TCP/IP thread (raw API). */
static void usbnic_iperf_start(void)
{
  if (s_iperf_session != NULL)
  {
    return;
  }

  s_iperf_session = lwiperf_start_tcp_server_default(usbnic_iperf_report, NULL);
  if (s_iperf_session != NULL)
  {
    LOG("IPERF: TCP server listening on port %u (run: iperf -c <board-ip>)",
        (unsigned)LWIPERF_TCP_PORT_DEFAULT);
  }
  else
  {
    LOG("IPERF: failed to start TCP server");
  }
}

/* TCP/IP-thread callbacks --------------------------------------------------*/

static void usbnic_do_bringup(void *arg)
{
  ip4_addr_t any;

  LWIP_UNUSED_ARG(arg);

  if (s_netif_added != 0U)
  {
    return;
  }

  ip4_addr_set_zero(&any);

  if (netif_add(&s_netif, &any, &any, &any, NULL,
                usbnic_netif_init_cb, tcpip_input) == NULL)
  {
    return;
  }

  netif_set_status_callback(&s_netif, usbnic_status_cb);
  netif_set_default(&s_netif);
  netif_set_up(&s_netif);

  if ((s_phost != NULL) && (USBH_RTKNIC_IsLinkUp(s_phost) != 0U))
  {
    netif_set_link_up(&s_netif);
  }

  (void)dhcp_start(&s_netif);
  LOG("NET: netif added, DHCP started");
  s_netif_added = 1U;

  /* Listen for iperf clients. The server binds to ANY and does not need an
     address yet; clients can connect once DHCP assigns one. */
  usbnic_iperf_start();
}

static void usbnic_do_linkchange(void *arg)
{
  uint8_t up = (uint8_t)(uint32_t)arg;

  if (s_netif_added == 0U)
  {
    return;
  }

  if (up != 0U)
  {
    netif_set_link_up(&s_netif);
    LOG("NET: link up");
  }
  else
  {
    netif_set_link_down(&s_netif);
    LOG("NET: link down");
  }
}

static void usbnic_do_remove(void *arg)
{
  LWIP_UNUSED_ARG(arg);

  if (s_netif_added == 0U)
  {
    return;
  }

  (void)dhcp_stop(&s_netif);
  netif_set_down(&s_netif);
  netif_remove(&s_netif);
  s_netif_added = 0U;
  LOG("NET: netif removed");
}

/* netif callbacks ----------------------------------------------------------*/

static err_t usbnic_netif_init_cb(struct netif *netif)
{
  netif->name[0] = 'u';
  netif->name[1] = '0';
  netif->output = etharp_output;
  netif->linkoutput = usbnic_linkoutput;
  netif->mtu = USBH_RTKNIC_GetMTU(s_phost);
  netif->hwaddr_len = RTKNIC_MAC_ADDR_LEN;

  if ((s_phost == NULL) || (USBH_RTKNIC_GetMacAddr(s_phost, netif->hwaddr) == 0U))
  {
    /* Should not happen - the class fabricates a MAC during bring-up. */
    netif->hwaddr[0] = 0x02U;
    netif->hwaddr[1] = 0x00U;
    netif->hwaddr[2] = 0x00U;
    netif->hwaddr[3] = 0x52U;
    netif->hwaddr[4] = 0x54U;
    netif->hwaddr[5] = 0x4BU;
  }

  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;

  LOG("NET: MAC=%02X:%02X:%02X:%02X:%02X:%02X MTU=%u",
      netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
      netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5],
      (unsigned)netif->mtu);

  return ERR_OK;
}

static err_t usbnic_linkoutput(struct netif *netif, struct pbuf *p)
{
  uint32_t len;
  USBH_StatusTypeDef st;

  LWIP_UNUSED_ARG(netif);

  if (p->tot_len > sizeof(s_txbuf))
  {
    return ERR_BUF;
  }

  len = pbuf_copy_partial(p, s_txbuf, p->tot_len, 0);
  if (len == 0U)
  {
    return ERR_BUF;
  }

  /* Non-blocking hand-off: the class enqueues the frame into its TX ring and
     the USB host thread sends it asynchronously. We must NOT block/spin on this
     thread - it is the TCP/IP thread, and stalling it here would hold back the
     ACK/window updates that keep the RX flow open (observed as a window-full
     sawtooth). If the ring is momentarily full we drop; TCP retransmits data
     and the next ACK supersedes a dropped pure-ACK. */
  st = USBH_RTKNIC_Transmit(s_phost, s_txbuf, len);
  if (st == USBH_BUSY)
  {
    return ERR_OK;   /* dropped - upper layers recover */
  }
  if (st != USBH_OK)
  {
    return ERR_IF;
  }

  return ERR_OK;
}

/* Read-only NIC snapshot for the GUI / diagnostics -------------------------*/
void usbnic_get_info(usbnic_info_t *out)
{
  USBH_HandleTypeDef *phost = s_phost;

  if (out == NULL)
  {
    return;
  }

  memset(out, 0, sizeof(*out));

  if (phost == NULL)
  {
    return;
  }

  out->present   = 1U;
  out->vid       = phost->device.DevDesc.idVendor;
  out->pid       = phost->device.DevDesc.idProduct;
  out->mtu       = USBH_RTKNIC_GetMTU(phost);
  out->proto     = (uint8_t)USBH_RTKNIC_GetProto(phost);
  out->mac_valid = USBH_RTKNIC_GetMacAddr(phost, out->mac);
  out->link_up   = USBH_RTKNIC_IsLinkUp(phost);
  (void)USBH_RTKNIC_GetLinkSpeed(phost, &out->speed_dl, &out->speed_ul);

  if ((s_netif_added != 0U) && netif_is_up(&s_netif))
  {
    out->ip4 = ip4_addr_get_u32(netif_ip4_addr(&s_netif));
  }
}

/* USB host class upcalls (override the weak defaults) ----------------------*/

void USBH_RTKNIC_OnActive(USBH_HandleTypeDef *phost)
{
  s_phost = phost;
  (void)tcpip_callback(usbnic_do_bringup, NULL);
}

void USBH_RTKNIC_OnLinkChange(USBH_HandleTypeDef *phost, uint8_t link_up)
{
  LWIP_UNUSED_ARG(phost);
  (void)tcpip_callback(usbnic_do_linkchange, (void *)(uint32_t)link_up);
}

void USBH_RTKNIC_OnDisconnect(USBH_HandleTypeDef *phost)
{
  LWIP_UNUSED_ARG(phost);
  if (s_netif_added != 0U)
  {
    (void)tcpip_callback(usbnic_do_remove, NULL);
  }
  s_phost = NULL;
}

void USBH_RTKNIC_OnRxFrame(USBH_HandleTypeDef *phost, uint8_t *frame, uint32_t len)
{
  struct pbuf *p;

  LWIP_UNUSED_ARG(phost);

  if ((s_netif_added == 0U) || (len == 0U))
  {
    return;
  }

  p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
  if (p == NULL)
  {
    return;
  }

  if (pbuf_take(p, frame, (u16_t)len) != ERR_OK)
  {
    (void)pbuf_free(p);
    return;
  }

  /* s_netif.input == tcpip_input: thread-safe hand-off to the TCP/IP thread. */
  if (s_netif.input(p, &s_netif) != ERR_OK)
  {
    (void)pbuf_free(p);
  }
}

/* Public API ---------------------------------------------------------------*/

void usbnic_lwip_init(void)
{
  tcpip_init(NULL, NULL);
}

/* lwIP random source -------------------------------------------------------*/
uint32_t usbnic_lwip_rand(void)
{
  static uint32_t state;

  /* Seed lazily from the system tick; mix with a linear congruential step. */
  if (state == 0U)
  {
    state = HAL_GetTick() ^ 0xA5A5F00DU;
  }
  state = (state * 1664525U) + 1013904223U;
  return state;
}
