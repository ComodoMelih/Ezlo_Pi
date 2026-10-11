/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    ezlopi_service_loop.c
 * @brief
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    Jan 24, 2023
 */
/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ezlopi_util_trace.h"

#include "ezlopi_core_processes.h"

#include "ezlopi_service_loop.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
/**
 * @brief Macro to get max len between two lengths
 *
 * @param str1_len
 * @param str2_len
 *
 */
#define MAX_LEN(str1_len, str2_len) ((str1_len > str2_len) ? str1_len : str2_len)

/**
 * @brief Linked list to store function loop details
 *
 */
typedef struct s_loop_node
{
    void *arg;                /**< Argument to be passed to the function loop when called */
    f_loop_t loop;            /**< Function loop */
    const char *name;         /**< Name for the function loop */
    uint32_t period_ms;       /**< Period ms for the function loop to be called */
    uint32_t _timer_ms;       /**< Timer linked to a speific function loop */
    bool _remove_pending;     /**< Set by EZPI_service_loop_remove; freed later by the loop task */
    struct s_loop_node *next; /**< Pointer to point to the next function loop detail hence becoming a linked list */
} s_loop_node_t;

/**
 * @brief Task that handles calling function loop
 *
 * @param pv Task argument
 */
static void ezpi_loop(void *pv);
/**
 * @brief Function to create a s_loop_node_t node and then return the pointer
 *
 * @param name Name for the function loop
 * @param loop Function loop
 * @param period_ms Period ms for the function loop to be called
 * @param arg Argument to be passed to the function loop when called
 * @return s_loop_node_t*
 * @retval Node pointer or NULL on error
 */
static s_loop_node_t *ezpi_create_node(const char *name, f_loop_t loop, uint32_t period_ms, void *arg);

/**
 * @brief Unlinks and frees every node flagged by EZPI_service_loop_remove.
 *
 * This is the only place a node is freed. Must be called from the loop task
 * only, and never while the list is being walked.
 */
static void ezpi_loop_reap(void);

/**
 * @brief Returns the mutex guarding __loop_head and the ->next chain,
 *        creating it on first use.
 *
 * @return SemaphoreHandle_t
 */
static SemaphoreHandle_t ezpi_loop_lock_get(void);

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static s_loop_node_t *__loop_head = NULL;

/* The list is mutated from more than one task. EZPI_service_loop_add is called
 * from app_main at boot (ezlopi_main.c:130, :138) and EZPI_service_loop_remove
 * is a registered cloud method -- EZPI_registered / EZPI_register_repeat
 * (ezlopi_core_api_macros.h:197, :207) -- so it runs on the cloud task while
 * ezpi_loop is walking the same chain. The walker task is created at
 * ezlopi_main.c:135, before EZPI_init() at :138 adds more nodes.
 *
 * Static allocation: CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y on all four
 * supported targets, and the lazy create below is first reached from
 * EZPI_service_led_indicator_init() while app_main is still the only task, so
 * the create itself does not need to be race-free. */
static SemaphoreHandle_t __loop_lock = NULL;
static StaticSemaphore_t __loop_lock_buffer;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
void EZPI_service_loop_add(const char *name, f_loop_t loop, uint32_t period_ms, void *arg)
{
    if (loop && name)
    {
        /* Allocated outside the lock: ezlopi_malloc can block on the heap
         * mutex, and holding the list lock across it would stall the loop
         * task for no reason. */
        s_loop_node_t *__new_node = ezpi_create_node(name, loop, pdMS_TO_TICKS(period_ms), arg);

        if (__new_node)
        {
            SemaphoreHandle_t lock = ezpi_loop_lock_get();

            if (lock)
            {
                xSemaphoreTake(lock, portMAX_DELAY);
            }

            if (__loop_head)
            {
                s_loop_node_t *__loop_node = __loop_head;
                while (__loop_node->next)
                {
                    __loop_node = __loop_node->next;
                }

                __loop_node->next = __new_node;
            }
            else
            {
                __loop_head = __new_node;
            }

            if (lock)
            {
                xSemaphoreGive(lock);
            }
        }
    }
}

void EZPI_service_loop_remove(f_loop_t loop)
{
    /* Mark only -- the node is freed later by ezpi_loop_reap() on the loop
     * task. Freeing here was unsafe in two independent ways:
     *
     *  1. Cross-task. This function is a registered cloud method, so it ran
     *     on the cloud task and freed a node that ezpi_loop was holding a
     *     pointer to, mid-walk.
     *  2. Self-removal. Several loop callbacks remove themselves while they
     *     are executing -- ping_sock.c:547, ping_sock.c:564, and the MQ
     *     sensor calibration callbacks such as
     *     sensor_0048_other_MQ4_CH4_detector.c:403. The old code freed the
     *     node the walker was standing on, and ezpi_loop then read
     *     __loop_node->next out of freed memory. A lock alone does not fix
     *     this case, because the caller already holds the walker's position.
     *
     * Clearing ->loop stops the callback from being invoked again from this
     * point on, which is the observable part of "remove". */
    if (loop)
    {
        SemaphoreHandle_t lock = ezpi_loop_lock_get();

        if (lock)
        {
            xSemaphoreTake(lock, portMAX_DELAY);
        }

        s_loop_node_t *__loop_node = __loop_head;
        while (__loop_node)
        {
            if (__loop_node->loop == loop)
            {
                /* First match only, matching the original semantics. */
                __loop_node->loop = NULL;
                __loop_node->_remove_pending = true;
                break;
            }

            __loop_node = __loop_node->next;
        }

        if (lock)
        {
            xSemaphoreGive(lock);
        }
    }
}

void EZPI_service_loop_init(void)
{
    TaskHandle_t ezlopi_service_timer_task_handle = NULL;
    xTaskCreate(ezpi_loop, "ezpi_loop", EZLOPI_SERVICE_LOOP_TASK_DEPTH, NULL, 4, &ezlopi_service_timer_task_handle);

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    EZPI_core_process_set_process_info(ENUM_EZLOPI_SERVICE_LOOP_TASK, &ezlopi_service_timer_task_handle, EZLOPI_SERVICE_LOOP_TASK_DEPTH);
#endif
}
/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static void ezpi_loop(void *pv)
{
    while (1)
    {
        uint32_t __run_time = xTaskGetTickCount();

        /* Reap between passes, never during one: this is the only point at
         * which no ->next pointer is being held across a callback. */
        ezpi_loop_reap();

        SemaphoreHandle_t lock = ezpi_loop_lock_get();

        if (lock)
        {
            xSemaphoreTake(lock, portMAX_DELAY);
        }
        s_loop_node_t *__loop_node = __loop_head;
        if (lock)
        {
            xSemaphoreGive(lock);
        }

        while (__loop_node)
        {

            uint32_t conditions = ((xTaskGetTickCount() - __loop_node->_timer_ms) >= __loop_node->period_ms);
            if ((NULL != __loop_node->loop) && conditions)
            {

                uint32_t __loop_time = xTaskGetTickCount();
                __loop_node->loop(__loop_node->arg);
                __loop_node->_timer_ms = xTaskGetTickCount();
                __loop_time = (xTaskGetTickCount() - __loop_time);

                // TRACE_D("'%s': \t\t %u(period: %d(cmp: %d))", __loop_node->name ? __loop_node->name : ezlopi__str, __loop_time);

                vTaskDelay(pdMS_TO_TICKS(1));
            }

            /* Read ->next under the lock. The callback above may have added
             * a node (appending writes the previous tail's ->next), and a
             * concurrent add from another task can do the same. The node
             * itself cannot have been freed, because only this task frees,
             * and only in ezpi_loop_reap() above. */
            if (lock)
            {
                xSemaphoreTake(lock, portMAX_DELAY);
            }
            __loop_node = __loop_node->next;
            if (lock)
            {
                xSemaphoreGive(lock);
            }
        }

        __run_time = xTaskGetTickCount() - __run_time;
        // TRACE_I("loop runtime: %u", __run_time);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void ezpi_loop_reap(void)
{
    SemaphoreHandle_t lock = ezpi_loop_lock_get();

    if (lock)
    {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    s_loop_node_t **pp = &__loop_head;
    while (*pp)
    {
        s_loop_node_t *__node = *pp;

        if (__node->_remove_pending)
        {
            *pp = __node->next;
            ezlopi_free(__FUNCTION__, __node);
        }
        else
        {
            pp = &__node->next;
        }
    }

    if (lock)
    {
        xSemaphoreGive(lock);
    }
}

static SemaphoreHandle_t ezpi_loop_lock_get(void)
{
    if (NULL == __loop_lock)
    {
        __loop_lock = xSemaphoreCreateMutexStatic(&__loop_lock_buffer);
    }

    return __loop_lock;
}

static s_loop_node_t *ezpi_create_node(const char *name, f_loop_t loop, uint32_t period_ms, void *arg)
{
    s_loop_node_t *__loop_node = ezlopi_malloc(__FUNCTION__, sizeof(s_loop_node_t));

    if (__loop_node)
    {
        __loop_node->arg = arg;
        __loop_node->name = name;
        __loop_node->loop = loop;
        __loop_node->_timer_ms = 0;
        __loop_node->period_ms = period_ms;
        __loop_node->_remove_pending = false;

        __loop_node->next = NULL;
    }

    return __loop_node;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
