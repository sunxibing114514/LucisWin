/* heap.h — Phase 2.3 进程内存分配器公开接口
 *
 * 提供 Win32 Heap/Virtual 系列函数背后的软件分配器。
 * 内存布局延续 Phase 1/2 的 host-malloc + guest-偏移模型:
 *   分配的块落在 [heap_base, heap_base + WINE_HEAP_MAX) 区间,
 *   guest 地址 = heap_base + offset, 解释器通过 mem_base + addr 直接访问。
 *
 * 单线程假设 (Phase 2), 无锁。
 */
#ifndef WINE_HEAP_H
#define WINE_HEAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WINE_HEAP_MAX: 动态分配区总大小 (字节), 由 core.c realloc 时使用 */
#define WINE_HEAP_MAX (4u * 1024u * 1024u)

/* wine_heap_reset: 重置分配器到指定 heap_base
 *   清空块表, bump 指针归零。每次 wine_run_exe 前调用。
 *   heap_base: guest 地址 (PE 镜像 + 栈空间之后) */
void wine_heap_reset(uint32_t heap_base);

/* ---- 以下函数供 thunk 直接调用, 返回 guest 地址 (0=失败) ---- */

/* HeapAlloc: 分配 size 字节, 8 字节对齐 */
uint32_t wine_heap_alloc(uint32_t size);

/* HeapFree: 释放 (仅标记 used=0, 不归还 bump)。
 *   返回 1=成功, 0=失败 (块不存在) */
int      wine_heap_free(uint32_t guest_addr);

/* HeapSize: 返回块的实际大小, 0=未找到 */
uint32_t wine_heap_size(uint32_t guest_addr);

/* VirtualAlloc: 分配 align(size, 4096) 字节, 4096 对齐
 *   addr==NULL 时从 bump 分配; addr!=NULL 时本 Phase 不支持, 返回 0 */
uint32_t wine_virtual_alloc(uint32_t addr, uint32_t size);

/* VirtualFree: MEM_RELEASE 时标记释放。返回 1=成功 */
int      wine_virtual_free(uint32_t guest_addr);

/* ---- 查询 (测试用) ---- */

/* wine_heap_get_base: 返回当前 heap_base */
uint32_t wine_heap_get_base(void);

#ifdef __cplusplus
}
#endif

#endif /* WINE_HEAP_H */
