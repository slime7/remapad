/**
 * 主机端测试用的最小 portmacro.h：临界区退化为空实现。
 *
 * 被测源码（dp_source.c）只用 portMUX 保护「注入按键的读改写」，主机测试
 * 是单线程顺序执行，不存在并发，因此空实现不会掩盖真实缺陷。
 */
#pragma once

typedef int portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

