#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/** @file
 * @brief FreeRTOS host-port configuration for tests, demos, and local apps.
 *
 * The configuration enables preemption, dynamic allocation, task
 * notifications, queues, and semaphores while disabling unused timer and trace
 * facilities. The one-megabyte heap accommodates desktop test workloads. */

#include <assert.h>
#include <stdint.h>

/* Native POSIX test configuration. Supply a target-specific config for firmware. */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      1000000UL
#define configTICK_RATE_HZ                      1000U
#define configMAX_PRIORITIES                    8U
#define configMINIMAL_STACK_SIZE                128U
#define configMAX_TASK_NAME_LEN                 24U
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (1024U * 1024U)
#define configAPPLICATION_ALLOCATED_HEAP        0

#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1U
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configGENERATE_RUN_TIME_STATS           0
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_POSIX_ERRNO                   0

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskAbortDelay                 1

#define configASSERT(condition) assert(condition)

#endif
