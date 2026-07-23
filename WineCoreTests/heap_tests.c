/* heap_tests.c — Phase 2.3 单元测试: 进程内存分配器
 *
 * 直接调用 wine_heap_* 接口 (不经 PE 解释器), 验证分配器基本语义:
 *   - reset 后 bump 归零, base 可查
 *   - HeapAlloc 返回非 0 指针且在 [base, base+HEAP_MAX) 区间
 *   - 分配的内存可通过 mem_base 读写 (此测试只测地址语义)
 *   - 8 字节对齐
 *   - HeapFree 标记释放后再分配不会返回同一地址 (bump 不回收)
 *   - VirtualAlloc 4096 对齐
 *   - HeapSize 返回正确大小
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/heap.h"
#include <stdint.h>
#include <string.h>

/* 测试用伪 mem_base (分配器只关心 guest 地址, 不实际读写) */
#define TEST_BASE 0x10000u

TEST(heap_reset_clears_bump) {
    wine_heap_reset(TEST_BASE);
    /* 分配一次拿到地址, 再 reset 应归零 */
    uint32_t a1 = wine_heap_alloc(16);
    ASSERT(a1 == TEST_BASE, "首次分配应在 base, 实际 %#x", a1);

    wine_heap_reset(TEST_BASE);
    uint32_t a2 = wine_heap_alloc(16);
    ASSERT(a2 == TEST_BASE, "reset 后应再次从 base 分配, 实际 %#x", a2);
}

TEST(heap_alloc_returns_in_range) {
    wine_heap_reset(TEST_BASE);
    uint32_t a = wine_heap_alloc(64);
    ASSERT(a >= TEST_BASE, "分配 %#x 低于 base %#x", a, TEST_BASE);
    ASSERT(a < TEST_BASE + WINE_HEAP_MAX, "分配 %#x 超出上限", a);
}

TEST(heap_alloc_8byte_aligned) {
    wine_heap_reset(TEST_BASE);
    for (int i = 0; i < 5; i++) {
        uint32_t a = wine_heap_alloc(3 + i * 7);  /* 故意非对齐大小 */
        ASSERT((a & 7u) == 0, "分配 %#x 未 8 字节对齐", a);
    }
}

TEST(heap_alloc_bump_advances) {
    wine_heap_reset(TEST_BASE);
    uint32_t a1 = wine_heap_alloc(16);  /* base+0..15 */
    uint32_t a2 = wine_heap_alloc(16);  /* base+16..31 */
    ASSERT(a1 == TEST_BASE, "a1 应在 base, 实际 %#x", a1);
    ASSERT(a2 == TEST_BASE + 16, "a2 应在 base+16, 实际 %#x", a2);
}

TEST(heap_free_marks_unused) {
    wine_heap_reset(TEST_BASE);
    uint32_t a = wine_heap_alloc(32);
    ASSERT(a != 0, "分配失败");
    int ok = wine_heap_free(a);
    ASSERT(ok == 1, "HeapFree 应返回 1, 实际 %d", ok);
    /* HeapSize 在 free 后应返回 0 (块未使用) */
    uint32_t sz = wine_heap_size(a);
    ASSERT(sz == 0, "free 后 HeapSize 应为 0, 实际 %u", sz);
}

TEST(heap_size_returns_alloc_size) {
    wine_heap_reset(TEST_BASE);
    uint32_t a = wine_heap_alloc(100);
    uint32_t sz = wine_heap_size(a);
    ASSERT(sz == 100, "HeapSize 应为 100, 实际 %u", sz);
}

TEST(heap_free_unknown_returns_zero) {
    wine_heap_reset(TEST_BASE);
    int ok = wine_heap_free(0xDEADBEEF);
    ASSERT(ok == 0, "释放未知地址应返回 0, 实际 %d", ok);
}

TEST(virtual_alloc_4096_aligned) {
    wine_heap_reset(TEST_BASE);
    /* 先做一次 heap alloc 占位, 看 virtual 是否仍对齐到 4096 */
    wine_heap_alloc(10);
    uint32_t v = wine_virtual_alloc(0, 100);
    ASSERT(v != 0, "VirtualAlloc 失败");
    ASSERT((v & 0xFFFu) == 0, "VirtualAlloc %#x 未 4096 对齐", v);
    ASSERT(v >= TEST_BASE, "VirtualAlloc %#x 低于 base", v);
}

TEST(virtual_alloc_size_aligned_to_page) {
    wine_heap_reset(TEST_BASE);
    uint32_t v1 = wine_virtual_alloc(0, 1);        /* 1 字节 -> 1 页 */
    uint32_t v2 = wine_virtual_alloc(0, 4096);      /* 1 页 */
    uint32_t v3 = wine_virtual_alloc(0, 4097);      /* >1 页 */
    ASSERT(v1 != 0 && v2 != 0 && v3 != 0, "分配失败");
    /* v1, v2 应连续差 1 页; v3 应差 2 页 (但 v2 仅占 1 页, 故 v3-v2 >= 4096) */
    ASSERT(v2 - v1 == 4096, "v2-v1 应为 4096, 实际 %u", v2 - v1);
    ASSERT(v3 - v2 >= 4096, "v3-v2 应 >=4096, 实际 %u", v3 - v2);
}

TEST(virtual_free_releases) {
    wine_heap_reset(TEST_BASE);
    uint32_t v = wine_virtual_alloc(0, 4096);
    ASSERT(v != 0, "VirtualAlloc 失败");
    int ok = wine_virtual_free(v);
    ASSERT(ok == 1, "VirtualFree 应返回 1, 实际 %d", ok);
}

TEST(heap_get_base_returns_set_value) {
    wine_heap_reset(0xABCDEF00);
    ASSERT(wine_heap_get_base() == 0xABCDEF00,
           "base 应为 0xABCDEF00, 实际 %#x", wine_heap_get_base());
}
