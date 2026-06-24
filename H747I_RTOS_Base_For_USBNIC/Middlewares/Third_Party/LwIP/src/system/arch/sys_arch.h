/**
  ******************************************************************************
  * @file    system/arch/sys_arch.h
  * @brief   lwIP OS abstraction types for CMSIS-RTOS v2 (FreeRTOS).
  ******************************************************************************
  */
#ifndef __SYS_ARCH_H__
#define __SYS_ARCH_H__

#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protection type for SYS_LIGHTWEIGHT_PROT (saved interrupt mask). */
typedef uint32_t sys_prot_t;

/* lwIP synchronization primitives mapped onto CMSIS-RTOS v2 objects. */
typedef osSemaphoreId_t   sys_sem_t;
typedef osMutexId_t       sys_mutex_t;
typedef osThreadId_t      sys_thread_t;

typedef struct {
  osMessageQueueId_t id;
} sys_mbox_t;

/* Validity helpers (used by lwIP when LWIP_COMPAT_MUTEX is 0). */
#define sys_mbox_valid(mbox)        (((mbox) != NULL) && ((mbox)->id != NULL))
#define sys_mbox_set_invalid(mbox)  do { if ((mbox) != NULL) { (mbox)->id = NULL; } } while (0)

#define sys_sem_valid(sem)          (((sem) != NULL) && (*(sem) != NULL))
#define sys_sem_set_invalid(sem)    do { if ((sem) != NULL) { *(sem) = NULL; } } while (0)

#define sys_mutex_valid(mutex)      (((mutex) != NULL) && (*(mutex) != NULL))
#define sys_mutex_set_invalid(mutex) do { if ((mutex) != NULL) { *(mutex) = NULL; } } while (0)

#ifdef __cplusplus
}
#endif

#endif /* __SYS_ARCH_H__ */
