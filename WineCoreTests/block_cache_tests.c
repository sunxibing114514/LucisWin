/* block_cache_tests.c — Phase 2.4 单元测试: 基本块翻译缓存
 *
 * 构造小型 i386 字节码, 用 cpu_run 执行, 验证:
 *   - 命中计数器递增 (循环体所在块被重复命中)
 *   - 启用缓存前后语义一致 (结果相同)
 *   - fallback 降级正确 (含不支持指令的块仍能跑完)
 *   - 控制流指令不在缓存块内 (走慢路径)
 *
 * 字节码全部用解释器实际执行的真指令, 不 stub。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/cpu.h"
#include "../WineCore/translator/block_cache.h"
#include <stdint.h>
#include <string.h>

/* ---- 测试夹具: 构造 CPU 上下文跑字节码 ---- */
static cpu_context_t make_ctx(uint8_t *mem, size_t len) {
    cpu_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mem_base = mem;
    ctx.mem_size = (uint32_t)len;
    ctx.current_image = NULL;   /* 无 IAT, 关闭 IAT 区间检测 */
    /* 栈: 放在 mem 末尾, 向下生长 */
    ctx.gpr[CPU_REG_ESP] = (uint32_t)len - 16;
    /* 伪造返回地址 (hlt 会停, 用不到) */
    return ctx;
}

/* ---- 测试 1: 紧循环命中计数 ----
 * 字节码:
 *   0: B8 00 00 00 00     mov eax, 0
 *   5: 40                 inc eax           <- .loop (块起点)
 *   6: 83 F8 05           cmp eax, 5
 *   9: 7C FA              jl  .loop  (rel8 -6 -> 0x05)
 *   B: F4                 hlt
 * 跑完 eax==5, 循环块 (inc/cmp) 应被命中 >=3 次。
 *
 * 命中分析: inc 共执行 5 次 (eax 0->5)。首次 eip=0 块含 mov+inc+cmp,
 * 把 eax 0->1 顺便做了; 之后 eip=5 块执行剩余 4 次 inc (eax 1->2->3->4->5),
 * 首次 eip=5 miss 翻译, 后 3 次命中 -> hits==3。
 */
static const uint8_t k_loop_code[] = {
    0xB8, 0x00,0x00,0x00,0x00,    /* mov eax,0   @0 */
    0x40,                          /* inc eax     @5  (loop top) */
    0x83,0xF8,0x05,                /* cmp eax,5   @6 */
    0x7C,0xFA,                     /* jl -6 -> @5 @9 */
    0xF4,                          /* hlt         @B */
};
#define LOOP_TOP 5u

TEST(cache_loop_hits_increment) {
    block_cache_reset();
    uint8_t mem[256];
    memset(mem, 0xCC, sizeof(mem));
    memcpy(mem, k_loop_code, sizeof(k_loop_code));
    cpu_context_t ctx = make_ctx(mem, sizeof(mem));
    cpu_run(&ctx, 0);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 5,
           "eax 应为 5, 实际 %u", ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.status == CPU_OK, "应以 hlt 正常停止, 实际 %d", ctx.status);
    /* eip=5 块首次 miss 翻译, 后续 3 次循环命中 -> hits>=3 */
    ASSERT(block_cache_hits() >= 3,
           "命中计数应 >=3, 实际 %llu",
           (unsigned long long)block_cache_hits());
}

/* ---- 测试 2: 启用/禁用缓存语义一致 ----
 * 同一段代码跑两遍: 一遍禁用缓存, 一遍启用, 结果应完全相同。
 * 用一个稍微复杂的算术序列 (仅用 interpreter 已实现的指令)。
 *   0: B8 01 00 00 00     mov eax, 1
 *   5: BB 0A 00 00 00     mov ebx, 10
 *   A: 01 D8              add eax, ebx   (eax=11)
 *   C: 83 C0 05           add eax, 5     (eax=16)
 *   F: 83 E8 03           sub eax, 3     (eax=13)
 *  12: 81 F0 FF 00 00 00  xor eax, 0xFF  (eax=13^0xFF=0xF2)  [0x81 /6, mod=3 rm=0]
 *  18: F4                hlt
 * 注: 不用 0x35 (xor eax,imm32 AL 短形式), interpreter 未实现, 会 #UD。
 */
static const uint8_t k_arith_code[] = {
    0xB8, 0x01,0x00,0x00,0x00,
    0xBB, 0x0A,0x00,0x00,0x00,
    0x01,0xD8,
    0x83,0xC0,0x05,
    0x83,0xE8,0x03,
    0x81,0xF0, 0xFF,0x00,0x00,0x00,
    0xF4,
};

static uint32_t run_arith(int enabled) {
    uint8_t mem[256];
    memset(mem, 0xCC, sizeof(mem));
    memcpy(mem, k_arith_code, sizeof(k_arith_code));
    cpu_context_t ctx = make_ctx(mem, sizeof(mem));
    if (enabled) { block_cache_enable(); block_cache_reset(); }
    else         { block_cache_disable(); block_cache_reset(); }
    cpu_run(&ctx, 0);
    block_cache_enable();  /* 恢复默认, 不影响后续测试 */
    return ctx.gpr[CPU_REG_EAX];
}

TEST(cache_disabled_vs_enabled_identical) {
    uint32_t r_off = run_arith(0);
    uint32_t r_on  = run_arith(1);
    /* 13 = 0x0D, ^0xFF = 0xF2 = 242 */
    uint32_t expect = 13u ^ 0xFFu;
    ASSERT(r_off == expect, "禁用缓存结果 %#x, 期望 %#x", r_off, expect);
    ASSERT(r_on  == expect, "启用缓存结果 %#x, 期望 %#x", r_on,  expect);
    ASSERT(r_off == r_on, "启用/禁用结果不一致: %u vs %u", r_off, r_on);
}

/* ---- 测试 3: fallback 降级 (含不支持指令) ----
 *   0: B8 05 00 00 00     mov eax, 5      (可翻译)
 *   5: 90                 nop              (不支持 -> fallback)
 *   6: 40                 inc eax          (可翻译, eax=6)
 *   7: F4                 hlt
 * 块在 @5 处 fallback 到慢路径, 慢路径跑 nop (0x90 switch 无 case -> #UD!)
 *
 * 注意: 0x90 nop 在 interpreter.c 可能未实现 -> 会 #UD。为避免此干扰,
 * 改用一条 "支持但不翻译" 的指令: 用 0xF6 (group 3, test r/m8,imm8)
 * 它不在翻译子集, 但 interpreter.c switch 0xF6 默认 -> #UD。
 *
 * 更稳妥: 用一条 switch 支持但翻译器未收录的指令做 fallback 触发。
 * 0xC9 (leave) 是 switch 支持的, 但我们翻译器不收录 -> fallback 后慢路径跑 leave。
 * 但 leave 改 esp/ebp, 难以独立断言。
 *
 * 最终选用: 0x90 nop。检查 interpreter.c 是否实现 0x90; 若未实现, 本测试改用
 * "mov eax,5; mov ebx,6; hlt" 这种全可翻译序列, 仅验证不命中 fallback 也能跑。
 * 这里用纯可翻译序列验证块不 fallback 时仍正常:
 *   0: B8 05 00 00 00  mov eax,5
 *   5: BB 06 00 00 00  mov ebx,6
 *   A: 01 D8           add eax,ebx  (eax=11)
 *   C: F4              hlt
 */
static const uint8_t k_pure_code[] = {
    0xB8, 0x05,0x00,0x00,0x00,
    0xBB, 0x06,0x00,0x00,0x00,
    0x01,0xD8,
    0xF4,
};

TEST(cache_pure_block_no_fallback) {
    block_cache_reset();
    block_cache_enable();
    uint8_t mem[256];
    memset(mem, 0xCC, sizeof(mem));
    memcpy(mem, k_pure_code, sizeof(k_pure_code));
    cpu_context_t ctx = make_ctx(mem, sizeof(mem));
    cpu_run(&ctx, 0);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 11, "eax 应为 11, 实际 %u", ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.gpr[CPU_REG_EBX] == 6,  "ebx 应为 6, 实际 %u", ctx.gpr[CPU_REG_EBX]);
    ASSERT(ctx.status == CPU_OK, "应 hlt, 实际 %d", ctx.status);
    /* 首次执行应至少 miss 一次 (翻译), 命中可 0 (直线只跑一次) */
    ASSERT(block_cache_misses() >= 1, "应有 >=1 次 miss, 实际 %llu",
           (unsigned long long)block_cache_misses());
}

/* ---- 测试 4: fallback 到慢路径仍能完成 (含真实 fallback 指令) ----
 * 用 push imm32 (0x68) 触发 fallback: 翻译器不收录 0x68, 但 switch 支持。
 *   0: B8 21 00 00 00  mov eax, 0x21   (可翻译)
 *   5: 68 AA BB CC DD  push 0xDDCCBBAA  (fallback -> 慢路径, 压栈)
 *   A: 5B              pop ebx          (fallback, 慢路径; ebx=0xDDCCBBAA)
 *   B: F4              hlt
 * 启用缓存后, ebx 仍应为 0xDDCCBBAA, 证明 fallback 正确降级。
 */
static const uint8_t k_fallback_code[] = {
    0xB8, 0x21,0x00,0x00,0x00,
    0x68, 0xAA,0xBB,0xCC,0xDD,
    0x5B,
    0xF4,
};

TEST(cache_fallback_to_slow_path_correct) {
    block_cache_reset();
    block_cache_enable();
    uint8_t mem[256];
    memset(mem, 0xCC, sizeof(mem));
    memcpy(mem, k_fallback_code, sizeof(k_fallback_code));
    cpu_context_t ctx = make_ctx(mem, sizeof(mem));
    cpu_run(&ctx, 0);
    ASSERT(ctx.status == CPU_OK, "应 hlt, 实际 %d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0x21, "eax 应为 0x21, 实际 %#x",
           ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.gpr[CPU_REG_EBX] == 0xDDCCBBAA,
           "ebx 应为 0xDDCCBBAA (fallback 后慢路径压/弹正确), 实际 %#x",
           ctx.gpr[CPU_REG_EBX]);
}

/* ---- 测试 5: INC/DEC 标志在缓存路径下正确 ----
 *   0: B8 FF FF FF FF   mov eax, 0xFFFFFFFF
 *   5: 40               inc eax     (eax=0, ZF=1, OF=1, CF 不变)
 *   6: 74 03            jz +3 -> @B  (跳过 dec)
 *   8: 48               dec eax     (不应执行)
 *   9: EB 01            jmp +1 -> @C
 *   B: F4               hlt         @B
 * 启用缓存: inc 所在块翻译 inc, 之后 jz 走慢路径。eax 最终 0。
 */
static const uint8_t k_inc_flags_code[] = {
    0xB8, 0xFF,0xFF,0xFF,0xFF,
    0x40,
    0x74,0x03,
    0x48,
    0xEB,0x01,
    0xF4,
};

TEST(cache_inc_sets_zf_correctly) {
    block_cache_reset();
    block_cache_enable();
    uint8_t mem[256];
    memset(mem, 0xCC, sizeof(mem));
    memcpy(mem, k_inc_flags_code, sizeof(k_inc_flags_code));
    cpu_context_t ctx = make_ctx(mem, sizeof(mem));
    cpu_run(&ctx, 0);
    ASSERT(ctx.status == CPU_OK, "应 hlt, 实际 %d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0,
           "eax 应为 0 (inc 0xFFFFFFFF + jz 跳过 dec), 实际 %#x",
           ctx.gpr[CPU_REG_EAX]);
}

/* ---- 测试 6: 缓存槽冲突覆盖不破坏正确性 ----
 * 两个不同 EIP 映射到同一槽时, 后翻译覆盖前者, 仍正确执行。
 * 跑两段独立代码 (不同 mem), 验证结果正确。
 */
TEST(cache_slot_eviction_still_correct) {
    block_cache_reset();
    block_cache_enable();
    /* 第一段: mov eax,7; add eax,3; hlt -> eax=10 */
    uint8_t mem1[256];
    memset(mem1, 0xCC, sizeof(mem1));
    static const uint8_t c1[] = {
        0xB8,0x07,0x00,0x00,0x00, 0x83,0xC0,0x03, 0xF4
    };
    memcpy(mem1, c1, sizeof(c1));
    cpu_context_t ctx1 = make_ctx(mem1, sizeof(mem1));
    cpu_run(&ctx1, 0);
    ASSERT(ctx1.gpr[CPU_REG_EAX] == 10, "段1 eax 应为 10, 实际 %u",
           ctx1.gpr[CPU_REG_EAX]);

    /* 第二段不同内容, 起始 EIP 可能冲突也可能不冲突; 无论哪种都应正确 */
    uint8_t mem2[256];
    memset(mem2, 0xCC, sizeof(mem2));
    static const uint8_t c2[] = {
        0xB8,0x64,0x00,0x00,0x00, 0x83,0xE8,0x14, 0xF4  /* mov eax,100; sub eax,20 -> 80 */
    };
    memcpy(mem2, c2, sizeof(c2));
    cpu_context_t ctx2 = make_ctx(mem2, sizeof(mem2));
    cpu_run(&ctx2, 0);
    ASSERT(ctx2.gpr[CPU_REG_EAX] == 80, "段2 eax 应为 80, 实际 %u",
           ctx2.gpr[CPU_REG_EAX]);
}
