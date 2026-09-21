/**
 * 主机端单元测试的最小支撑：断言宏 + 用例表。
 *
 * 这些测试把与硬件无关的固件模块（NS2 编码、序列号、帧构造、像素加速回调、
 * 输入源合成）编译到开发机上直接运行，秒级出结果，不需要上板。
 *
 * 为什么不用 ESP-IDF 自带的 Unity：那套要在目标芯片上跑，依赖串口与实板；
 * 这里要的是「改一行就能立刻重跑」的回归网。断言宏足够小，读起来也不比
 * 框架多一层。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 单个用例。失败用 CHECK/REQUIRE 记录，函数返回即用例结束。 */
typedef void (*host_test_fn)(void);

typedef struct {
    const char *name;
    host_test_fn fn;
} host_test_case_t;

typedef struct {
    const char *name;
    const host_test_case_t *cases;
    size_t count;
} host_test_suite_t;

/** 记录一次断言结果；返回是否成立，供 REQUIRE 直接 return。 */
bool host_test_expect(bool ok, const char *detail, const char *file, int line);

void host_test_expect_u64(uint64_t actual, uint64_t expected, const char *detail,
                          const char *file, int line);
void host_test_expect_bytes(const void *actual, const void *expected, size_t len,
                            const char *detail, const char *file, int line);

/** 全部测试套件，NULL 结尾。定义在 firmware/test/suites.c。 */
const host_test_suite_t *const *host_test_all_suites(void);

void host_test_run(const host_test_suite_t *suite);

/** 断言：失败继续往下跑，用例末尾统一报失败。 */
#define CHECK(cond) host_test_expect((cond) ? true : false, #cond, __FILE__, __LINE__)
/** 断言：失败立刻结束本用例（后续依赖前提，继续跑没有意义）。 */
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        if (!host_test_expect((cond) ? true : false, #cond, __FILE__, __LINE__)) { \
            return;                                                         \
        }                                                                   \
    } while (0)

#define CHECK_EQ(actual, expected) \
    host_test_expect_u64((uint64_t)(actual), (uint64_t)(expected), #actual " == " #expected, \
                         __FILE__, __LINE__)
/** 逐字节比较：编码器这类定长输出用它，任何一位错位都会报出来。 */
#define CHECK_BYTES(actual, expected, len) \
    host_test_expect_bytes((actual), (expected), (len), #actual, __FILE__, __LINE__)

/** 一个测试文件导出一个 suite，注册表在 firmware/test/suites.c。 */
#define HOST_TEST_SUITE(symbol, suite_name, ...)                       \
    static const host_test_case_t symbol##_cases[] = { __VA_ARGS__ };  \
    const host_test_suite_t symbol = {                                 \
        suite_name, symbol##_cases,                                    \
        sizeof(symbol##_cases) / sizeof(symbol##_cases[0])};
