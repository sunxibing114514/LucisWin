/* block_cache.h — Phase 2.4 基本块翻译缓存公开接口
 *
 * 预解码 i386 基本块为 uop 序列, 重复执行时跳过逐字节解码。
 * 仅翻译 fib.exe 热循环用到的指令子集 (mov/alu/inc/dec); 其余一律
 * UOP_FALLBACK 降级到既有 switch 解释器, 保证语义不变。
 *
 * 安全保证: 缓存只加速, 不改语义。详见
 * docs/superpowers/specs/2026-07-23-luciswin-phase2.4-blockcache-design.md
 */
#ifndef WINE_BLOCK_CACHE_H
#define WINE_BLOCK_CACHE_H

#include <stdint.h>
#include "wine/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 缓存查找 + 执行
 *
 * block_cache_try_exec: 主循环在查 switch 前调用。
 *   返回 1 = 命中并已执行 (ctx->eip 已推进到块末, 主循环 continue);
 *   返回 0 = 未命中/未执行, 主循环走原 switch 慢路径。
 *   命中后若块尾是 UOP_FALLBACK, ctx->eip 已设为 fallback 指令起点。 */
int block_cache_try_exec(cpu_context_t *ctx, uint32_t eip);

/* ============================================================
 * 观测 (测试用)
 * ============================================================ */
uint64_t block_cache_hits(void);
uint64_t block_cache_misses(void);

/* block_cache_reset: 清空缓存与计数器 (测试间隔离) */
void block_cache_reset(void);

/* block_cache_disable / enable: 单测对比启用前后语义用 (默认启用) */
void block_cache_disable(void);
void block_cache_enable(void);
int  block_cache_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* WINE_BLOCK_CACHE_H */
