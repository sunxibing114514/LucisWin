/* heap.c — Phase 2.3 内存管理实现 (GREEN)
 *
 * 进程内存分配器: HeapAlloc/HeapFree/HeapSize + VirtualAlloc/VirtualFree。
 *
 * 内存模型延续 Phase 1/2.x 的 host-malloc + guest-偏移:
 *   分配的块落在 [heap_base, heap_base + WINE_HEAP_MAX) 区间,
 *   guest 地址 = heap_base + offset, 解释器用 mem_base + addr 直接访问。
 *
 * 算法:
 *   - bump allocator: HeapAlloc 推进 g_bump, 返回 heap_base + offset
 *   - 块元数据表 (宿主侧): 记录每块的 (guest_addr, size, used, is_virtual)
 *   - HeapFree: 仅标记 used=0, 不归还 bump (简单实现, 够 Phase 2.3 测试)
 *   - VirtualAlloc: 4096 对齐, 用同 bump 推进 (标记 is_virtual=1)
 *   - VirtualFree: 标记 used=0
 *
 * 单线程假设 (Phase 2), 无锁。
 *
 * Win32 thunk 实现:
 *   各 thunk 从 guest 栈读参数 (stdcall [ESP+4] 起), 调本文件分配器,
 *   返回值写 gpr[EAX]。thunk 入口栈布局 (主循环 FF /2 压了 ret_addr):
 *     [ESP] = ret_addr, [ESP+4] = 第一个参数, [ESP+8] = 第二个, ...
 */
#include "wine/cpu.h"
#include "wine/heap.h"
#include <string.h>

/* ---- 块元数据表 ---- */
#define WINE_HEAP_BLOCK_MAX 256
typedef struct {
    uint32_t guest_addr;   /* 分配的 guest 地址 */
    uint32_t size;         /* 分配大小 (字节) */
    int      used;         /* 1=已分配, 0=空闲 */
    int      is_virtual;   /* 1=VirtualAlloc, 0=HeapAlloc */
} heap_block_t;

static heap_block_t g_blocks[WINE_HEAP_BLOCK_MAX];

/* bump 指针 (相对 heap_base 的偏移) */
static uint32_t g_bump = 0;
static uint32_t g_heap_base = 0;

/* ---- 对齐辅助 ---- */
static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* ---- 在块表找槽 ---- */
static int find_slot_by_addr(uint32_t addr) {
    for (int i = 0; i < WINE_HEAP_BLOCK_MAX; i++) {
        if (g_blocks[i].used && g_blocks[i].guest_addr == addr) return i;
    }
    return -1;
}
static int find_free_slot(void) {
    for (int i = 0; i < WINE_HEAP_BLOCK_MAX; i++) {
        if (!g_blocks[i].used) return i;
    }
    return -1;
}

/* ============================================================
 * 公开 API
 * ============================================================ */

void wine_heap_reset(uint32_t heap_base) {
    g_heap_base = heap_base;
    g_bump = 0;
    memset(g_blocks, 0, sizeof(g_blocks));
}

uint32_t wine_heap_get_base(void) {
    return g_heap_base;
}

uint32_t wine_heap_alloc(uint32_t size) {
    if (size == 0) return 0;
    uint32_t aligned = align_up(size, 8);
    if (g_bump + aligned > WINE_HEAP_MAX) return 0;

    int slot = find_free_slot();
    if (slot < 0) return 0;

    uint32_t addr = g_heap_base + g_bump;
    g_bump += aligned;

    g_blocks[slot].guest_addr = addr;
    g_blocks[slot].size = size;       /* 记录用户请求大小 (HeapSize 返回此值) */
    g_blocks[slot].used = 1;
    g_blocks[slot].is_virtual = 0;
    return addr;
}

int wine_heap_free(uint32_t guest_addr) {
    int slot = find_slot_by_addr(guest_addr);
    if (slot < 0) return 0;
    g_blocks[slot].used = 0;
    return 1;
}

uint32_t wine_heap_size(uint32_t guest_addr) {
    int slot = find_slot_by_addr(guest_addr);
    if (slot < 0) return 0;
    return g_blocks[slot].size;
}

uint32_t wine_virtual_alloc(uint32_t addr, uint32_t size) {
    /* Phase 2.3 仅支持 addr==NULL 的自动分配 */
    if (addr != 0) return 0;
    if (size == 0) return 0;

    /* 4096 对齐 bump 到页边界 */
    g_bump = align_up(g_bump, 4096);
    uint32_t aligned = align_up(size, 4096);
    if (g_bump + aligned > WINE_HEAP_MAX) return 0;

    int slot = find_free_slot();
    if (slot < 0) return 0;

    uint32_t a = g_heap_base + g_bump;
    g_bump += aligned;

    g_blocks[slot].guest_addr = a;
    g_blocks[slot].size = size;
    g_blocks[slot].used = 1;
    g_blocks[slot].is_virtual = 1;
    return a;
}

int wine_virtual_free(uint32_t guest_addr) {
    int slot = find_slot_by_addr(guest_addr);
    if (slot < 0) return 0;
    g_blocks[slot].used = 0;
    return 1;
}

/* ============================================================
 * kernel32 thunk: 从 guest 栈读参数, 调分配器, 写 EAX
 *
 * stdcall 栈布局 (thunk 入口 [ESP]=ret_addr, 主循环 FF /2 压):
 *   [ESP+0]  ret_addr
 *   [ESP+4]  arg1
 *   [ESP+8]  arg2
 *   ...
 * ============================================================ */

/* GetProcessHeap() — 无参数, 返回伪堆句柄 1 */
void kernel32_GetProcessHeap_thunk(cpu_context_t *ctx) {
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* HeapAlloc(heap, dwFlags, dwBytes) — 返回 guest 指针 */
void kernel32_HeapAlloc_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    /* uint32_t heap = cpu_mem_r32(ctx, esp + 4); */      /* 不用 */
    /* uint32_t flags = cpu_mem_r32(ctx, esp + 8); */     /* 不用 */
    uint32_t bytes = cpu_mem_r32(ctx, esp + 12);
    ctx->gpr[CPU_REG_EAX] = wine_heap_alloc(bytes);
}

/* HeapFree(heap, dwFlags, lpMem) — 返回 TRUE/FALSE */
void kernel32_HeapFree_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    /* uint32_t heap = cpu_mem_r32(ctx, esp + 4); */
    /* uint32_t flags = cpu_mem_r32(ctx, esp + 8); */
    uint32_t lp = cpu_mem_r32(ctx, esp + 12);
    ctx->gpr[CPU_REG_EAX] = (uint32_t)wine_heap_free(lp);
}

/* HeapSize(heap, dwFlags, lpMem) — 返回字节数 */
void kernel32_HeapSize_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t lp = cpu_mem_r32(ctx, esp + 12);
    ctx->gpr[CPU_REG_EAX] = wine_heap_size(lp);
}

/* HeapCreate(flOptions, dwInitialSize, dwMaximumSize) — 返回伪堆句柄 */
void kernel32_HeapCreate_thunk(cpu_context_t *ctx) {
    /* Phase 2.3: 所有堆都走同一分配器, 返回递增伪句柄 (2,3,...) */
    static uint32_t s_next_handle = 2;
    ctx->gpr[CPU_REG_EAX] = s_next_handle++;
}

/* HeapDestroy(heap) — Phase 2.3 空实现, 返回 TRUE */
void kernel32_HeapDestroy_thunk(cpu_context_t *ctx) {
    ctx->gpr[CPU_REG_EAX] = 1;
}

/* VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect)
 *   返回 guest 指针, 0=失败 */
void kernel32_VirtualAlloc_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t addr = cpu_mem_r32(ctx, esp + 4);
    uint32_t size = cpu_mem_r32(ctx, esp + 8);
    /* flAllocationType = [esp+12], flProtect = [esp+16] — Phase 2.3 不解析 */
    ctx->gpr[CPU_REG_EAX] = wine_virtual_alloc(addr, size);
}

/* VirtualFree(lpAddress, dwSize, dwFreeType) — 返回 TRUE/FALSE */
void kernel32_VirtualFree_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t addr = cpu_mem_r32(ctx, esp + 4);
    /* dwSize = [esp+8], dwFreeType = [esp+12] — 不解析 */
    ctx->gpr[CPU_REG_EAX] = (uint32_t)wine_virtual_free(addr);
}

/* VirtualQuery(lpAddress, lpBuffer, dwLength) — 写 MEMORY_BASIC_INFORMATION
 * Phase 2.3 返回写入字节数 (固定 28 字节), 简单填 BaseAddress/RegionSize */
void kernel32_VirtualQuery_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t addr = cpu_mem_r32(ctx, esp + 4);
    uint32_t buf_ga = cpu_mem_r32(ctx, esp + 8);
    /* MEMORY_BASIC_INFORMATION (32 位, 共 28 字节):
     *   BaseAddress(4), AllocationBase(4), AllocationProtect(4),
     *   RegionSize(4), State(4), Protect(4), Type(4) */
    if (buf_ga) {
        uint8_t *buf = ctx->mem_base + buf_ga;
        memset(buf, 0, 28);
        memcpy(buf + 0, &addr, 4);       /* BaseAddress */
        uint32_t region = 4096;
        memcpy(buf + 12, &region, 4);    /* RegionSize */
        uint32_t state = 0x1000;         /* MEM_COMMIT */
        memcpy(buf + 16, &state, 4);
    }
    ctx->gpr[CPU_REG_EAX] = 28;
}

/* RtlZeroMemory(dest, len) — 填零 */
void kernel32_RtlZeroMemory_thunk(cpu_context_t *ctx) {
    uint32_t esp = ctx->gpr[CPU_REG_ESP];
    uint32_t dest = cpu_mem_r32(ctx, esp + 4);
    uint32_t len = cpu_mem_r32(ctx, esp + 8);
    if (dest && len) memset(ctx->mem_base + dest, 0, len);
    /* RtlZeroMemory 无返回值 (void), 但栈是 stdcall ret 8 */
    ctx->gpr[CPU_REG_EAX] = 0;
}

/* GetTickCount() — 返回伪 tick (单调递增计数) */
void kernel32_GetTickCount_thunk(cpu_context_t *ctx) {
    static uint32_t s_tick = 0;
    s_tick += 10;
    ctx->gpr[CPU_REG_EAX] = s_tick;
}
