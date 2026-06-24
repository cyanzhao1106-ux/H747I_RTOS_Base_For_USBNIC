/**
  ******************************************************************************
  * @file    usbnic_netif.h
  * @author  USB NIC integration
  * @brief   lwIP network interface glue for the Realtek USB NIC host class.
  ******************************************************************************
  */

#ifndef __USBNIC_NETIF_H
#define __USBNIC_NETIF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbh_core.h"

/* Read-only snapshot of the currently attached USB NIC, for GUI/diagnostics.
   All fields are zeroed when no device is present (out->present == 0). */
typedef struct
{
  uint8_t  present;     /* 1 if a USB NIC is enumerated and active            */
  uint8_t  link_up;     /* 1 if the Ethernet link is up                       */
  uint8_t  mac_valid;   /* 1 if mac[] holds a valid station address           */
  uint8_t  mac[6];      /* station MAC address                                */
  uint16_t vid;         /* USB idVendor                                       */
  uint16_t pid;         /* USB idProduct                                      */
  uint16_t mtu;         /* link MTU (bytes)                                   */
  uint8_t  proto;       /* RTKNIC_PROTO_ECM / RTKNIC_PROTO_NCM                */
  uint32_t speed_dl;    /* downlink speed (bps); 0xFFFFFFFF if >4.29 Gbps     */
  uint32_t speed_ul;    /* uplink speed (bps)                                 */
  uint32_t ip4;         /* assigned IPv4, raw lwIP u32 (octet 1 in LSB); 0=none*/
} usbnic_info_t;

/* Fill *out with the current NIC snapshot. Safe to call from any task. */
void usbnic_get_info(usbnic_info_t *out);

/* Initialise the lwIP stack (must be called once, after the RTOS kernel is
   running and before the USB device is brought up). */
void usbnic_lwip_init(void);

/* Random number source used by lwIP (LWIP_RAND in lwipopts.h). */
uint32_t usbnic_lwip_rand(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBNIC_NETIF_H */
