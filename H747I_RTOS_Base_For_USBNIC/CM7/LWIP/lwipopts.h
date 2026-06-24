/**
  ******************************************************************************
  * @file    lwipopts.h
  * @brief   lwIP 2.2.1 options for the STM32H747I-DISCO USB-NIC RTOS project.
  *
  *          Stack model : NO_SYS = 0 (runs on top of CMSIS-RTOS v2 / FreeRTOS)
  *          API surface : RAW + netconn + BSD sockets
  *          Protocols   : ARP, IPv4, ICMP, UDP, TCP, DHCP client, DNS
  *
  *          The data path is a USB CDC-ECM/NCM network interface, which offers
  *          no checksum offload, so all checksums are computed in software.
  ******************************************************************************
  */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

/*---------------------------------------------------------------------------*/
/* System / OS                                                               */
/*---------------------------------------------------------------------------*/
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TIMERS                     1
#define LWIP_FREERTOS_CHECK_CORE_LOCKING 1

/* The OS port provides its own dynamic objects; lwIP uses its own heap. */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4

/*---------------------------------------------------------------------------*/
/* Memory                                                                    */
/*---------------------------------------------------------------------------*/
#define MEM_SIZE                        (24 * 1024)

#define MEMP_NUM_PBUF                   24
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                10
#define MEMP_NUM_TCP_PCB_LISTEN         6
#define MEMP_NUM_TCP_SEG                48
#define MEMP_NUM_REASSDATA              8
#define MEMP_NUM_FRAG_PBUF              16
#define MEMP_NUM_ARP_QUEUE              10
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define MEMP_NUM_NETBUF                 8
#define MEMP_NUM_NETCONN                12
#define MEMP_NUM_TCPIP_MSG_API          16
/* Deeper input queue so RX bursts are not dropped before the TCP/IP thread
   drains them. */
#define MEMP_NUM_TCPIP_MSG_INPKT        32

/* Packet buffer pool (full-MTU frames for the USB NIC). Sized to hold the full
   TCP receive window (16*MSS) in flight plus headroom for the input queue. */
#define PBUF_POOL_SIZE                  40
#define PBUF_POOL_BUFSIZE               1536

/*---------------------------------------------------------------------------*/
/* Protocols                                                                 */
/*---------------------------------------------------------------------------*/
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0

#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        1

#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0

/*---------------------------------------------------------------------------*/
/* TCP tuning                                                                */
/*---------------------------------------------------------------------------*/
#define TCP_MSS                         1460
/* Large RX window: throughput in the PC->board direction is bounded by how
   many bytes the sender may keep in flight while the board processes frames.
   16*MSS (~23 KB) hides the per-transfer USB + software-checksum latency.
   Backed by the enlarged PBUF_POOL below. */
#define TCP_WND                         (16 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_SACK_OUT               0

/*---------------------------------------------------------------------------*/
/* netif options                                                             */
/*---------------------------------------------------------------------------*/
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_API                  1
#define ETHARP_TABLE_SIZE               10
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define ETH_PAD_SIZE                    0

/*---------------------------------------------------------------------------*/
/* Sequential-layer / socket API                                             */
/*---------------------------------------------------------------------------*/
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define SO_REUSE                        1
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_COMPAT_MUTEX               0
#define LWIP_COMPAT_MUTEX_ALLOWED       0

/*---------------------------------------------------------------------------*/
/* Checksums - all in software (USB NIC has no offload)                      */
/*---------------------------------------------------------------------------*/
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

/*---------------------------------------------------------------------------*/
/* Threads (tcpip + sequential API). Stack sizes are in bytes.               */
/*---------------------------------------------------------------------------*/
#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          (4 * 1024)
#define TCPIP_THREAD_PRIO               (osPriorityNormal + 1)
#define TCPIP_MBOX_SIZE                 32
#define DEFAULT_THREAD_STACKSIZE        (2 * 1024)
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       16
#define DEFAULT_ACCEPTMBOX_SIZE         8
#define DEFAULT_RAW_RECVMBOX_SIZE       8

/*---------------------------------------------------------------------------*/
/* Randomness (DHCP XID, TCP ISN, ...). Replace with an RNG-backed source    */
/* if available; the port supplies a weak default otherwise.                 */
/*---------------------------------------------------------------------------*/
#include <stdint.h>
uint32_t usbnic_lwip_rand(void);
#define LWIP_RAND()                     (usbnic_lwip_rand())

/*---------------------------------------------------------------------------*/
/* Statistics / debug                                                        */
/*---------------------------------------------------------------------------*/
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG                      0

#endif /* __LWIPOPTS_H__ */
