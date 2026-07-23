/* fib.c — Phase 2.1 测试夹具
 *
 * 功能: 迭代计算 fib(20)=6765, 自写 itoa 拼接字符串后 MessageBoxA 显示。
 *
 * 此文件由 mingw (i686-w64-mingw32-gcc) 编译为 i386 PE (fib.exe),
 * 作为 WineCoreTests 的端到端测试输入, 验证解释器扩展后的指令集
 * (乘除法/移位/符号扩展/串操作/近 Jcc 等) 能完整跑通。
 *
 * 字符串全部 ASCII, 不涉及 Shift-JIS (Phase 1 已验证代码页转换)。
 */

#include <windows.h>

/* mingw -nostartfiles 模式下需要提供 CRT 运行时重定位器的空 stub */
void _pei386_runtime_relocator(void) {}

/* 自写 itoa: 无符号 -> 字符串, 避免 msvcrt sprintf 依赖
 *   val: 待转换的无符号整数
 *   buf: 输出缓冲区 (至少 12 字节, 32 位最大 10 位数字 + 符号 + \0)
 *   返回: buf 指针 */
static char *my_itoa(unsigned int val, char *buf, int base) {
    char tmp[16];
    int i = 0, j = 0;
    if (val == 0) { buf[0]='0'; buf[1]=0; return buf; }
    while (val > 0) {
        int d = val % base;                       /* 除法/取模驱动 DIV/IDIV */
        tmp[i++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
        val /= base;
    }
    while (i > 0) buf[j++] = tmp[--i];            /* 数组回填 */
    buf[j] = 0;
    return buf;
}

/* 迭代法 fibonacci: 驱动循环/比较/加法/条件跳转 */
static unsigned int fib(unsigned int n) {
    if (n < 2) return n;                          /* JL/JGE 近跳转 */
    unsigned int a = 0, b = 1;
    for (unsigned int i = 2; i <= n; i++) {       /* JLE 近跳转 */
        unsigned int c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(void)
{
    unsigned int n = 20;
    unsigned int r = fib(n);                       /* 6765 */
    char buf[64];
    char *p = buf;

    /* 手工拼接 "fib(20)=6765" — 驱动串复制 (字节 load/store) */
    const char *s = "fib(";
    while (*s) *p++ = *s++;                       /* MOVZX r32, r/m8 + STOSB 模式 */
    my_itoa(n, p, 10);                            /* "20" */
    while (*p) p++;                               /* 扫到串尾 */
    *p++ = ')';                                   /* 闭合括号 */
    *p++ = '=';
    my_itoa(r, p, 10);                            /* "6765" */

    /* 调用 user32!MessageBoxA — 端到端钩子拦截点 */
    MessageBoxA(NULL, buf, "luciswin", MB_OK);

    /* 调用 kernel32!ExitProcess — 解释器 longjmp 跳回 wine_run_exe */
    ExitProcess(0);
    return 0;
}
