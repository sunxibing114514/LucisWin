/* block_cache.c — Phase 2.4 基本块翻译缓存 (RED 占位)
 *
 * 完整实现见 GREEN 阶段。当前为 RED 占位:
 *   block_cache_try_exec 永远返回 0 (未命中), 计数器不递增,
 *   使 WineCoreTests/block_cache_tests.c 中的命中断言失败。
 */
#include "block_cache.h"

static uint64_t g_hits   = 0;
static uint64_t g_misses = 0;
static int      g_enabled = 1;

int block_cache_try_exec(cpu_context_t *ctx, uint32_t eip) {
    (void)ctx; (void)eip;
    return 0;  /* RED: 永远未命中 */
}

uint64_t block_cache_hits(void)   { return g_hits; }
uint64_t block_cache_misses(void) { return g_misses; }

void block_cache_reset(void) {
    g_hits = 0;
    g_misses = 0;
}

void block_cache_disable(void) { g_enabled = 0; }
void block_cache_enable(void)  { g_enabled = 1; }
int  block_cache_enabled(void) { return g_enabled; }
