/* test_framework.h — 极简 C 测试框架
 *
 * 功能: 提供断言宏与测试注册/运行机制, 不依赖 XCTest,
 *       同一份代码在 Linux(gcc) 与 macOS(xcodebuild) 上编译运行。
 *
 * 设计:
 *   TEST(name)        定义一个测试函数并自动注册 (constructor 自动调用)
 *   ASSERT(cond,...) 断言失败则记录并返回
 *   RUN_ALL_TESTS()   在 main 中调用, 返回失败数
 *
 * 注意: 注册表变量在 test_registry.c 中单一定义 (外部链接),
 *       各测试 .c 通过 extern 共享, 避免 static 多副本问题。 */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>

/* 全局失败计数 (在 test_registry.c 定义) */
extern int g_test_failures;
extern const char *g_current_test;

typedef void (*test_fn)(void);

#define TEST_MAX 64
extern test_fn g_tests[TEST_MAX];
extern const char *g_test_names[TEST_MAX];
extern int g_test_count;

/* 注册函数 (test_registry.c 实现) */
void test_register(const char *name, test_fn fn);

/* 定义测试: constructor 在 main 前自动注册 */
#define TEST(name) \
    static void name(void); \
    __attribute__((constructor)) static void name##_register(void) { \
        test_register(#name, name); \
    } \
    static void name(void)

/* 断言: 失败打印位置并从测试函数返回 */
#define ASSERT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] %s:%d: %s -- ", \
                g_current_test, __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_test_failures++; \
        return; \
    } \
} while (0)

/* 运行所有已注册测试, 返回失败数 */
static inline int RUN_ALL_TESTS(void) {
    int passed = 0;
    for (int i = 0; i < g_test_count; i++) {
        g_current_test = g_test_names[i];
        printf("[ RUN      ] %s\n", g_current_test);
        int before = g_test_failures;
        g_tests[i]();
        if (g_test_failures == before) {
            printf("[       OK ] %s\n", g_current_test);
            passed++;
        } else {
            printf("[  FAILED  ] %s\n", g_current_test);
        }
    }
    printf("\n%d/%d passed, %d failed\n", passed, g_test_count, g_test_failures);
    return g_test_failures;
}

#endif /* TEST_FRAMEWORK_H */
