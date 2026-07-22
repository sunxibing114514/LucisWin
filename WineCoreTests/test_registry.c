/* test_registry.c — 测试注册表单一定义
 *
 * test_framework.h 用 extern 声明全局变量, 此文件提供唯一定义,
 * 避免各测试 .c 各持 static 副本导致注册丢失。 */
#include "test_framework.h"

int g_test_failures = 0;
const char *g_current_test = "(none)";
test_fn g_tests[TEST_MAX];
const char *g_test_names[TEST_MAX];
int g_test_count = 0;

void test_register(const char *name, test_fn fn) {
    if (g_test_count < TEST_MAX) {
        g_test_names[g_test_count] = name;
        g_tests[g_test_count] = fn;
        g_test_count++;
    }
}
