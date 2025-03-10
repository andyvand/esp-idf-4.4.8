/* Tests for FreeRTOS scheduler suspend & resume all tasks */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "soc/cpu.h"
#include "test_utils.h"

#include "sdkconfig.h"

#include "esp_rom_sys.h"

#ifdef CONFIG_IDF_TARGET_ESP32S2
#define int_clr_timers int_clr
#define update update.update
#define int_st_timers int_st
#endif

static SemaphoreHandle_t isr_semaphore;

typedef struct {
    SemaphoreHandle_t trigger_sem;
    volatile unsigned counter;
} counter_config_t;

static void counter_task_fn(void *vp_config)
{
    counter_config_t *config = (counter_config_t *)vp_config;
    printf("counter_task running...\n");
    while(1) {
        xSemaphoreTake(config->trigger_sem, portMAX_DELAY);
        config->counter++;
    }
}

/* This test verifies that an interrupt can wake up a task while the scheduler is disabled.

   In the FreeRTOS implementation, this exercises the xPendingReadyList for that core.
 */
TEST_CASE("Scheduler disabled can handle a pending context switch on resume", "[freertos]")
{
    isr_semaphore = xSemaphoreCreateBinary();
    TaskHandle_t counter_task;

    counter_config_t count_config = {
        .trigger_sem = isr_semaphore,
        .counter = 0,
    };
    xTaskCreatePinnedToCore(counter_task_fn, "counter", 2048,
                            &count_config, UNITY_FREERTOS_PRIORITY + 1,
                            &counter_task, UNITY_FREERTOS_CPU);

    // Allow the counter task to spin up
    vTaskDelay(5);

    // Unblock the counter task and verify that it runs normally when the scheduler is running
    xSemaphoreGive(isr_semaphore);
    vTaskDelay(5);

    TEST_ASSERT(count_config.counter == 1);

    // Suspend the scheduler on this core
    vTaskSuspendAll();
    TEST_ASSERT_EQUAL(taskSCHEDULER_SUSPENDED, xTaskGetSchedulerState());

    unsigned no_sched_task = count_config.counter;

    // We simulate unblocking of the counter task from an ISR by
    // giving the isr_semaphore via the FromISR() API while the
    // scheduler is suspended. This prompts the kernel to put the
    // unblocked task on the xPendingReadyList.
    portBASE_TYPE yield = pdFALSE;
    xSemaphoreGiveFromISR(isr_semaphore, &yield);
    if (yield == pdTRUE) {
        portYIELD_FROM_ISR();
    }

    // scheduler off on this CPU...
    esp_rom_delay_us(20 * 1000);

    // Verify that the counter task is not scheduled when the scheduler is supended
    TEST_ASSERT_EQUAL(count_config.counter, no_sched_task);

    // When we resume scheduler, we expect the counter task
    // will preempt and count at least one more item
    xTaskResumeAll();
    TEST_ASSERT_EQUAL(taskSCHEDULER_RUNNING, xTaskGetSchedulerState());

    // Verify that the counter task has run after the scheduler is resumed
    TEST_ASSERT_NOT_EQUAL(count_config.counter, no_sched_task);

    // Clean up
    vTaskDelete(counter_task);
    vSemaphoreDelete(isr_semaphore);

    // Give the idle task a chance to cleanup any remaining deleted tasks
    vTaskDelay(10);
}

/* Multiple tasks on different cores can be added to the pending ready list
   while scheduler is suspended, and should be started once the scheduler
   resumes.
*/
TEST_CASE("Scheduler disabled can wake multiple tasks on resume", "[freertos]")
{
    #define TASKS_PER_PROC 4
    TaskHandle_t tasks[portNUM_PROCESSORS][TASKS_PER_PROC] = { 0 };
    counter_config_t counters[portNUM_PROCESSORS][TASKS_PER_PROC] = { 0 };

    /* Start all the tasks, they will block on isr_semaphore */
    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            counters[p][t].trigger_sem = xSemaphoreCreateMutex();
            TEST_ASSERT_NOT_NULL( counters[p][t].trigger_sem );
            TEST_ASSERT( xSemaphoreTake(counters[p][t].trigger_sem, 0) );
            xTaskCreatePinnedToCore(counter_task_fn, "counter", 2048,
                                    &counters[p][t], UNITY_FREERTOS_PRIORITY + 1,
                                    &tasks[p][t], p);
            TEST_ASSERT_NOT_NULL( tasks[p][t] );
        }
    }

    /* takes a while to initialize tasks on both cores, sometimes... */
    vTaskDelay(TASKS_PER_PROC * portNUM_PROCESSORS * 3);

    /* Check nothing is counting, each counter should be blocked on its trigger_sem */
    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            TEST_ASSERT_EQUAL(0, counters[p][t].counter);
        }
    }

    /* Suspend scheduler on this CPU */
    vTaskSuspendAll();

    /* Give all the semaphores once. This will wake tasks immediately on the other
       CPU, but they are deferred here until the scheduler resumes.
     */
    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            xSemaphoreGive(counters[p][t].trigger_sem);
        }
   }

    esp_rom_delay_us(200); /* Let the other CPU do some things */

    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            int expected = (p == UNITY_FREERTOS_CPU) ? 0 : 1; // Has run if it was on the other CPU
            esp_rom_printf("Checking CPU %d task %d (expected %d actual %d)\n", p, t, expected, counters[p][t].counter);
            TEST_ASSERT_EQUAL(expected, counters[p][t].counter);
        }
    }

    /* Resume scheduler */
    xTaskResumeAll();

    /* Now the tasks on both CPUs should have been woken once and counted once. */
    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            esp_rom_printf("Checking CPU %d task %d (expected 1 actual %d)\n", p, t, counters[p][t].counter);
            TEST_ASSERT_EQUAL(1, counters[p][t].counter);
        }
    }

    /* Clean up */
    for (int p = 0; p < portNUM_PROCESSORS; p++) {
        for (int t = 0; t < TASKS_PER_PROC; t++) {
            vTaskDelete(tasks[p][t]);
            vSemaphoreDelete(counters[p][t].trigger_sem);
        }
    }
}

#ifndef CONFIG_FREERTOS_UNICORE
static volatile bool sched_suspended;
static void suspend_scheduler_5ms_task_fn(void *ignore)
{
    vTaskSuspendAll();
    sched_suspended = true;
    for (int i = 0; i <5; i++) {
        esp_rom_delay_us(1000);
    }
    xTaskResumeAll();
    sched_suspended = false;
    vTaskDelete(NULL);
}

/* If the scheduler is disabled on one CPU (A) with a task blocked on something, and a task
   on B (where scheduler is running) wakes it, then the task on A should be woken on resume.
*/
TEST_CASE("Scheduler disabled on CPU B, tasks on A can wake", "[freertos]")
{
    TaskHandle_t counter_task;
    SemaphoreHandle_t wake_sem = xSemaphoreCreateMutex();
    xSemaphoreTake(wake_sem, 0);
    counter_config_t count_config = {
        .trigger_sem = wake_sem,
        .counter = 0,
    };
    xTaskCreatePinnedToCore(counter_task_fn, "counter", 2048,
                            &count_config, UNITY_FREERTOS_PRIORITY + 1,
                            &counter_task, !UNITY_FREERTOS_CPU);

    xTaskCreatePinnedToCore(suspend_scheduler_5ms_task_fn, "suspender", 2048,
                            NULL, UNITY_FREERTOS_PRIORITY - 1,
                            NULL, !UNITY_FREERTOS_CPU);

    /* counter task is now blocked on other CPU, waiting for wake_sem, and we expect
     that this CPU's scheduler will be suspended for 5ms shortly... */
    while(!sched_suspended) { }

    xSemaphoreGive(wake_sem);
    esp_rom_delay_us(1000);
    // Bit of a race here if the other CPU resumes its scheduler, but 5ms is a long time... */
    TEST_ASSERT(sched_suspended);
    TEST_ASSERT_EQUAL(0, count_config.counter); // the other task hasn't woken yet, because scheduler is off
    TEST_ASSERT(sched_suspended);

    /* wait for the rest of the 5ms... */
    while(sched_suspended) { }

    esp_rom_delay_us(100);
    TEST_ASSERT_EQUAL(1, count_config.counter); // when scheduler resumes, counter task should immediately count

    vTaskDelete(counter_task);
}
#endif
