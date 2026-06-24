/**
  ******************************************************************************
  * @file    system/OS/sys_arch.c
  * @brief   lwIP operating-system abstraction layer for CMSIS-RTOS v2
  *          (FreeRTOS kernel). Implements semaphores, mutexes, mailboxes,
  *          threads and the protection primitives required by lwIP when
  *          NO_SYS == 0.
  ******************************************************************************
  */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/mem.h"
#include "lwip/stats.h"
#include "cmsis_os2.h"
#include "cmsis_compiler.h"

/* Storage for the errno that lwIP provides (LWIP_PROVIDE_ERRNO in arch/cc.h).
   lwIP declares `extern int errno;`; this is its single definition. Kept
   outside the !NO_SYS guard so it exists regardless of OS configuration. */
int errno;

#if !NO_SYS

/* Number of pointer-sized slots used when the caller passes size 0. */
#ifndef SYS_MBOX_DEFAULT_SIZE
#define SYS_MBOX_DEFAULT_SIZE   8
#endif

/*---------------------------------------------------------------------------*/
/* Initialisation                                                            */
/*---------------------------------------------------------------------------*/
void sys_init(void)
{
  /* Nothing to initialise: the CMSIS-RTOS kernel is already started by the
     application before tcpip_init() is called. */
}

/*---------------------------------------------------------------------------*/
/* Time base                                                                 */
/*---------------------------------------------------------------------------*/
u32_t sys_now(void)
{
  /* The CMSIS-RTOS tick is 1000 Hz in this project, so ticks == milliseconds.
     Query the kernel tick frequency at runtime so this stays correct even if
     the tick rate is changed, without depending on FreeRTOSConfig.h here. */
  uint32_t ticks = osKernelGetTickCount();
  uint32_t freq  = osKernelGetTickFreq();
  if (freq == 1000U) {
    return (u32_t)ticks;
  }
  return (u32_t)((uint64_t)ticks * 1000U / freq);
}

u32_t sys_jiffies(void)
{
  return (u32_t)osKernelGetTickCount();
}

/*---------------------------------------------------------------------------*/
/* Critical-section / lightweight protection                                 */
/*---------------------------------------------------------------------------*/
sys_prot_t sys_arch_protect(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return (sys_prot_t)primask;
}

void sys_arch_unprotect(sys_prot_t pval)
{
  __set_PRIMASK((uint32_t)pval);
}

/*---------------------------------------------------------------------------*/
/* Semaphores (counting, used by lwIP as binary signals)                     */
/*---------------------------------------------------------------------------*/
err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
  *sem = osSemaphoreNew(1, (uint32_t)count, NULL);
  if (*sem == NULL)
  {
    SYS_STATS_INC(sem.err);
    return ERR_MEM;
  }
  SYS_STATS_INC_USED(sem);
  return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
  /* Releasing a binary semaphore that is already available simply fails;
     that is harmless for lwIP's signalling semantics. */
  (void)osSemaphoreRelease(*sem);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
  uint32_t start = osKernelGetTickCount();
  osStatus_t status;

  if (timeout == 0U)
  {
    status = osSemaphoreAcquire(*sem, osWaitForever);
  }
  else
  {
    status = osSemaphoreAcquire(*sem, timeout);
  }

  if (status != osOK)
  {
    return SYS_ARCH_TIMEOUT;
  }

  return (u32_t)(osKernelGetTickCount() - start);
}

void sys_sem_free(sys_sem_t *sem)
{
  if ((sem != NULL) && (*sem != NULL))
  {
    osSemaphoreDelete(*sem);
    SYS_STATS_DEC(sem.used);
    *sem = NULL;
  }
}

/*---------------------------------------------------------------------------*/
/* Mutexes                                                                   */
/*---------------------------------------------------------------------------*/
err_t sys_mutex_new(sys_mutex_t *mutex)
{
  *mutex = osMutexNew(NULL);
  if (*mutex == NULL)
  {
    SYS_STATS_INC(mutex.err);
    return ERR_MEM;
  }
  SYS_STATS_INC_USED(mutex);
  return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
  osMutexAcquire(*mutex, osWaitForever);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
  osMutexRelease(*mutex);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
  if ((mutex != NULL) && (*mutex != NULL))
  {
    osMutexDelete(*mutex);
    SYS_STATS_DEC(mutex.used);
    *mutex = NULL;
  }
}

/*---------------------------------------------------------------------------*/
/* Mailboxes (queues of void* messages)                                      */
/*---------------------------------------------------------------------------*/
err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
  if (size <= 0)
  {
    size = SYS_MBOX_DEFAULT_SIZE;
  }

  mbox->id = osMessageQueueNew((uint32_t)size, sizeof(void *), NULL);
  if (mbox->id == NULL)
  {
    SYS_STATS_INC(mbox.err);
    return ERR_MEM;
  }
  SYS_STATS_INC_USED(mbox);
  return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
  /* Block until there is room: lwIP requires post to never fail. */
  while (osMessageQueuePut(mbox->id, &msg, 0U, osWaitForever) != osOK)
  {
    osDelay(1);
  }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
  if (osMessageQueuePut(mbox->id, &msg, 0U, 0U) == osOK)
  {
    return ERR_OK;
  }
  SYS_STATS_INC(mbox.err);
  return ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
  if (osMessageQueuePut(mbox->id, &msg, 0U, 0U) == osOK)
  {
    return ERR_OK;
  }
  return ERR_MEM;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
  uint32_t start = osKernelGetTickCount();
  void *tmp = NULL;
  osStatus_t status;

  status = osMessageQueueGet(mbox->id, &tmp, NULL,
                             (timeout == 0U) ? osWaitForever : timeout);
  if (status != osOK)
  {
    if (msg != NULL)
    {
      *msg = NULL;
    }
    return SYS_ARCH_TIMEOUT;
  }

  if (msg != NULL)
  {
    *msg = tmp;
  }
  return (u32_t)(osKernelGetTickCount() - start);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
  void *tmp = NULL;

  if (osMessageQueueGet(mbox->id, &tmp, NULL, 0U) != osOK)
  {
    if (msg != NULL)
    {
      *msg = NULL;
    }
    return SYS_MBOX_EMPTY;
  }

  if (msg != NULL)
  {
    *msg = tmp;
  }
  return 0U;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
  if ((mbox != NULL) && (mbox->id != NULL))
  {
    osMessageQueueDelete(mbox->id);
    SYS_STATS_DEC(mbox.used);
    mbox->id = NULL;
  }
}

/*---------------------------------------------------------------------------*/
/* Threads                                                                   */
/*---------------------------------------------------------------------------*/
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio)
{
  osThreadAttr_t attr = {0};

  /* lwIP passes stack sizes in bytes via lwipopts.h (e.g. TCPIP_THREAD_STACKSIZE).
     Use a safe fixed fallback in bytes if a non-positive size is supplied, so we
     do not depend on FreeRTOSConfig.h here. */
  attr.name       = name;
  attr.stack_size = (stacksize > 0) ? (uint32_t)stacksize : 1024U;
  attr.priority   = (osPriority_t)prio;

  return osThreadNew((osThreadFunc_t)thread, arg, &attr);
}

#endif /* !NO_SYS */
