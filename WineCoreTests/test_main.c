/* test_main.c — 测试入口, 运行所有已注册测试
 *
 * 通过 test_framework.h 的 constructor 机制自动收集测试,
 * 这里只需调用 wine_init() 注册内建 DLL (让导入解析测试能验证真实 IAT 填充),
 * 再调用 RUN_ALL_TESTS()。
 * 返回非零表示有失败, CI 据此判定。 */
#include "test_framework.h"
#include "wine/wine.h"

int main(void) {
    /* 注册内建 DLL 导出, 让 pe_loader 的导入解析能填充真实函数地址 */
    wine_init();
    return RUN_ALL_TESTS() == 0 ? 0 : 1;
}
