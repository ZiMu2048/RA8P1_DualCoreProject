/*
 * FreeRTOS 应用钩子。
 *
 * 该文件属于用户代码，不会在 FSP 重新生成工程时被覆盖。
 */

#include "FreeRTOS.h"
#include "task.h"

/*
 * 栈溢出后可在调试器 Watch 窗口查看这两个变量，定位发生故障的线程。
 * 变量刻意保留为全局 volatile，避免编译器优化掉故障现场信息。
 */
volatile TaskHandle_t g_stack_overflow_task;
char const * volatile g_stack_overflow_task_name;

void vApplicationStackOverflowHook(TaskHandle_t x_task, char * p_task_name)
{
    g_stack_overflow_task      = x_task;
    g_stack_overflow_task_name = p_task_name;

    /*
     * 此时当前线程的栈已经不可信，不能继续调用 printf、申请内存或
     * 执行其他依赖任务栈的恢复逻辑。停止系统以保护执行器状态和现场。
     */
    taskDISABLE_INTERRUPTS();

    for (;;)
    {
        __asm volatile ("nop");
    }
}
