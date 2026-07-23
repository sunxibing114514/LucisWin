/* heap.c — Phase 2.3 测试夹具
 *
 * 验证 luciswin 的内存管理路径:
 *   GetProcessHeap → HeapAlloc (写 "heap ") →
 *   VirtualAlloc (写 "ok") → 拼接 → MessageBoxA("heap ok") →
 *   HeapFree / VirtualFree → ExitProcess(0)
 *
 * 此文件由 mingw (i686-w64-mingw32-gcc) 编译为 i386 PE (heap.exe),
 * 作为 WineCoreTests 的端到端测试输入。
 */
#include <windows.h>

void _pei386_runtime_relocator(void) {}

int main(void) {
    /* 1. HeapAlloc: 分配 64 字节, 写入 "heap " */
    HANDLE heap = GetProcessHeap();
    char *p1 = (char *)HeapAlloc(heap, 0, 64);
    if (!p1) ExitProcess(1);
    p1[0] = 'h'; p1[1] = 'e'; p1[2] = 'a'; p1[3] = 'p'; p1[4] = ' '; p1[5] = 0;

    /* 2. VirtualAlloc: 预留+提交 1 页, 写入 "ok" */
    char *p2 = (char *)VirtualAlloc(NULL, 4096,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p2) ExitProcess(2);
    p2[0] = 'o'; p2[1] = 'k'; p2[2] = 0;

    /* 3. 拼接到 p1 末尾: "heap ok" */
    char *dst = p1 + 5;     /* 跳过 "heap " */
    dst[0] = p2[0];         /* 'o' */
    dst[1] = p2[1];         /* 'k' */
    dst[2] = 0;

    /* 4. 读回验证 (内部一致性) */
    if (p1[0] != 'h' || p2[1] != 'k') ExitProcess(3);

    /* 5. 显示结果 (先显示再释放, 避免 UAF) */
    MessageBoxA(NULL, p1, "luciswin", MB_OK);

    /* 6. 释放 */
    HeapFree(heap, 0, p1);
    VirtualFree(p2, 0, MEM_RELEASE);

    ExitProcess(0);
    return 0;
}
