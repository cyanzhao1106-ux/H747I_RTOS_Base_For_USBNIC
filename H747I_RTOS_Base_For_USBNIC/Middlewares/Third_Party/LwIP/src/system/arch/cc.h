/**
  ******************************************************************************
  * @file    system/arch/cc.h
  * @brief   lwIP compiler/architecture abstraction for STM32H7 + Arm Compiler 6
  *          (armclang) / GCC. Part of the USB-NIC RTOS integration.
  ******************************************************************************
  */
#ifndef __CC_H__
#define __CC_H__

#include <stdint.h>
#include <stdio.h>

/* The Arm C library's <errno.h> only defines a few values (EDOM/ERANGE/...),
   not the BSD socket set (ENOBUFS, EWOULDBLOCK, ...) that lwIP's sockets/err
   code needs. Let lwIP define the full errno set; the `int errno;` storage is
   provided by the port (see sys_arch.c). */
#define LWIP_PROVIDE_ERRNO      1

/* Little endian Cortex-M7 */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* Structure packing - Arm Compiler 6 (armclang) and GCC both accept the
   GNU __attribute__((packed)) syntax. */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
/* Attribute-based packing above is self-contained; do NOT define
   PACK_STRUCT_USE_INCLUDES (that would require arch/bpstruct.h + epstruct.h). */

/* Diagnostics / assert hooks */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)

#define LWIP_PLATFORM_ASSERT(x) \
  do { printf("LWIP ASSERT: %s @ %s:%d\r\n", (x), __FILE__, __LINE__); \
       while (1) {} } while (0)

/* Random number hook (used when LWIP_RAND is referenced). A weak default is
   provided in the port; applications may override LWIP_RAND in lwipopts.h. */
#endif /* __CC_H__ */
