/**
 * 主机端测试运行器：逐个用例执行、打印结果，失败数作为进程退出码。
 */
#include "host_test.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char *s_case = NULL;
static unsigned s_case_failures = 0;
static unsigned s_total_failures = 0;
static unsigned s_total_passed = 0;

bool host_test_expect(bool ok, const char *detail, const char *file, int line)
{
  if (ok) {
    return true;
  }
  s_case_failures++;
  printf("    FAIL %s:%d  %s\n", file, line, detail);
  return false;
}

void host_test_expect_u64(uint64_t actual, uint64_t expected, const char *detail, const char *file, int line)
{
  if (actual == expected) {
    return;
  }
  s_case_failures++;
  printf("    FAIL %s:%d  %s: 期望 0x%llx（%llu），实际 0x%llx（%llu）\n", file, line, detail,
         (unsigned long long)expected, (unsigned long long)expected, (unsigned long long)actual,
         (unsigned long long)actual);
}

void host_test_expect_bytes(const void *actual, const void *expected, size_t len, const char *detail, const char *file,
                            int line)
{
  const uint8_t *got = (const uint8_t *)actual;
  const uint8_t *want = (const uint8_t *)expected;
  if (memcmp(got, want, len) == 0) {
    return;
  }
  s_case_failures++;
  printf("    FAIL %s:%d  %s: 首个不同字节\n", file, line, detail);
  for (size_t index = 0; index < len; index++) {
    if (got[index] == want[index]) {
      continue;
    }
    printf("      偏移 0x%02zx（第 %zu 字节）：期望 0x%02x，实际 0x%02x\n", index, index + 1, want[index], got[index]);
    break;
  }
}

void host_test_run(const host_test_suite_t *suite)
{
  printf("\n▶ %s\n", suite->name);
  for (size_t index = 0; index < suite->count; index++) {
    const host_test_case_t *item = &suite->cases[index];
    s_case = item->name;
    s_case_failures = 0;
    item->fn();
    if (s_case_failures == 0) {
      s_total_passed++;
      printf("  ✓ %s\n", s_case);
    } else {
      s_total_failures += s_case_failures;
      printf("  ✗ %s（%u 处断言失败）\n", s_case, s_case_failures);
    }
  }
  (void)s_case;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
#if defined(_WIN32)
  SetConsoleOutputCP(65001);
#endif
  const host_test_suite_t *const *suites = host_test_all_suites();
  for (size_t index = 0; suites[index] != NULL; index++) {
    host_test_run(suites[index]);
  }
  printf("\n固件主机端测试：%u 个用例通过", s_total_passed);
  if (s_total_failures == 0) {
    printf("，全部通过\n");
    return 0;
  }
  printf("，%u 处断言失败\n", s_total_failures);
  return 1;
}
