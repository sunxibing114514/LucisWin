/* interpreter_tests.c — i386 解释器单指令测试 (RED 阶段)
 *
 * 策略: 手工构造 machine code 字节序列, 在极简 CPU 上下文中执行,
 *       断言执行后 GPR / EFLAGS / ESP 的状态符合 x86 语义。
 *
 * 终止约定: 代码末尾放 0xF4 (hlt) 让解释器正常停止返回 CPU_OK。
 *           call/ret 测试靠 ret 跳回 hlt 终止。
 *
 * 内存模型: mem_base 指向共享缓冲区, 代码在起始, 栈在尾部 (ESP = MEM_SIZE - 64)。
 *           所有 guest 地址 = mem_base 偏移。
 *
 * 测试列表 (对应 spec §6.2):
 *   - mov eax, imm32
 *   - push imm8 / pop eax (栈平衡)
 *   - xor eax, eax (ZF 置位)
 *   - add eax, ebx (标志位)
 *   - sub esp, imm8 (hello.exe 实际用到)
 *   - jmp rel8 (跳过填充字节)
 *   - jz rel8 taken / not taken
 *   - call rel32 / ret (ESP 平衡)
 */
#include "test_framework.h"
#include "wine/cpu.h"
#include "wine/wine.h"
#include <string.h>

/* 测试用内存大小 */
#define TEST_MEM_SIZE 8192
/* 初始 ESP (栈顶偏移, 留 64 字节余量) */
#define TEST_ESP_INIT (TEST_MEM_SIZE - 64)

/* 共享内存缓冲区 (代码 + 栈) */
static uint8_t g_mem[TEST_MEM_SIZE];

/* run_code_raw: 不清零 ctx, 调用方可预设 gpr/eflags 后调用 */
static cpu_status_t run_code_raw(const uint8_t *code, size_t len,
                                  cpu_context_t *ctx) {
    memset(g_mem, 0, sizeof(g_mem));
    memcpy(g_mem, code, len);
    /* 确保末尾有 hlt */
    if (len < TEST_MEM_SIZE) g_mem[len] = 0xF4;

    /* 共享 fake image 禁用 IAT 区间判断 */
    static pe_image_t img;
    memset(&img, 0, sizeof(img));
    img.iat_base = 0xFFFFFFFF; /* 超大值, 永不在 IAT 区间 */
    img.iat_size = 0;

    ctx->mem_base = g_mem;
    ctx->mem_size = TEST_MEM_SIZE;
    ctx->eip = 0;
    if (ctx->gpr[CPU_REG_ESP] == 0) ctx->gpr[CPU_REG_ESP] = TEST_ESP_INIT;
    ctx->current_image = &img;
    ctx->status = CPU_OK;

    return cpu_run(ctx, 0);
}

/* run_code: 清零 ctx 后执行 (默认初始状态) */
static cpu_status_t run_code(const uint8_t *code, size_t len,
                              cpu_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return run_code_raw(code, len, ctx);
}

/* ---- 测试用例 ---- */

/* mov eax, 0x12345678 → EAX == 0x12345678 */
TEST(interp_mov_eax_imm32) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12, /* mov eax, 0x12345678 */
        0xF4,                          /* hlt */
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0x12345678,
           "EAX=%#x 期望 0x12345678", ctx.gpr[CPU_REG_EAX]);
}

/* push 0x10; pop eax → EAX==0x10 且 ESP 复位 */
TEST(interp_push_imm8_pop_eax) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0x6A, 0x10, /* push 0x10 */
        0x58,        /* pop eax */
        0xF4,
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0x10,
           "EAX=%#x 期望 0x10", ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.gpr[CPU_REG_ESP] == TEST_ESP_INIT,
           "ESP 未复位: %#x 期望 %#x", ctx.gpr[CPU_REG_ESP], TEST_ESP_INIT);
}

/* xor eax, eax → EAX==0 且 ZF 置位 */
TEST(interp_xor_eax_eax_sets_zf) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0x31, 0xC0, /* xor eax, eax */
        0xF4,
    };
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[CPU_REG_EAX] = 0xDEAD; /* 预设非零, 验证 xor 清零 */
    run_code_raw(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0, "EAX 非零: %#x", ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.eflags & (1u << CPU_FLAG_ZF), "ZF 未置位, eflags=%#x", ctx.eflags);
}

/* add eax, ebx: 1+1=2, ZF 清除 */
TEST(interp_add_eax_ebx) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0x01, 0xD8, /* add eax, ebx (ModRM D8 = reg=EBX rm=EAX) */
        0xF4,
    };
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[CPU_REG_EAX] = 1;
    ctx.gpr[CPU_REG_EBX] = 1;
    run_code_raw(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 2, "EAX=%#x 期望 2", ctx.gpr[CPU_REG_EAX]);
    ASSERT(!(ctx.eflags & (1u << CPU_FLAG_ZF)),
           "ZF 不应置位, eflags=%#x", ctx.eflags);
}

/* add eax, 0xFF: 1+0xFF=0x100, CF 置位 (无符号溢出) */
TEST(interp_add_sets_carry) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0x83, 0xC0, 0xFF, /* add eax, -1 (imm8 sign-extended) */
        0xF4,
    };
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[CPU_REG_EAX] = 1;
    run_code_raw(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0, "EAX=%#x 期望 0", ctx.gpr[CPU_REG_EAX]);
    ASSERT(ctx.eflags & (1u << CPU_FLAG_CF),
           "CF 未置位, eflags=%#x", ctx.eflags);
}

/* sub esp, 0x10 → ESP 减少 0x10 */
TEST(interp_sub_esp_imm8) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        0x83, 0xEC, 0x10, /* sub esp, 0x10 */
        0xF4,
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_ESP] == TEST_ESP_INIT - 0x10,
           "ESP=%#x 期望 %#x", ctx.gpr[CPU_REG_ESP], TEST_ESP_INIT - 0x10);
}

/* jmp rel8: 跳过 mov eax,0xFF, 验证 EAX 未被赋值 */
TEST(interp_jmp_rel8) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        /* [0] */ 0xEB, 0x05,          /* jmp +5 → 跳到 offset 7 */
        /* [2] */ 0xB8, 0xFF, 0x00, 0x00, 0x00, /* mov eax, 0xFF */
        /* [7] */ 0xF4,               /* hlt */
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0,
           "EAX=%#x 期望 0 (jmp 未跳过 mov)", ctx.gpr[CPU_REG_EAX]);
}

/* jz rel8 taken: ZF 预置, 跳过 mov eax,0xFF */
TEST(interp_jz_rel8_taken) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        /* [0] */ 0x74, 0x05,          /* jz +5 → 跳到 offset 7 */
        /* [2] */ 0xB8, 0xFF, 0x00, 0x00, 0x00, /* mov eax, 0xFF */
        /* [7] */ 0xF4,
    };
    memset(&ctx, 0, sizeof(ctx));
    ctx.eflags = (1u << CPU_FLAG_ZF); /* 预置 ZF */
    run_code_raw(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0,
           "EAX=%#x 期望 0 (jz 未跳转)", ctx.gpr[CPU_REG_EAX]);
}

/* jz rel8 not taken: ZF 清除, 落入 mov eax,0xFF */
TEST(interp_jz_rel8_not_taken) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        /* [0] */ 0x74, 0x05,          /* jz +5 */
        /* [2] */ 0xB8, 0xFF, 0x00, 0x00, 0x00, /* mov eax, 0xFF */
        /* [7] */ 0xF4,
    };
    memset(&ctx, 0, sizeof(ctx));
    ctx.eflags = 0; /* ZF 清除 */
    run_code_raw(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0xFF,
           "EAX=%#x 期望 0xFF (jz 误跳转)", ctx.gpr[CPU_REG_EAX]);
}

/* call rel32; ret: ESP 平衡 (call 压栈, ret 弹栈) */
TEST(interp_call_ret_balanced) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        /* [0] */ 0xE8, 0x01, 0x00, 0x00, 0x00, /* call +1 → offset 6 */
        /* [5] */ 0xF4,                         /* hlt (返回点) */
        /* [6] */ 0xC3,                         /* ret */
        /* [7] */ 0xF4,                         /* safety hlt */
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_ESP] == TEST_ESP_INIT,
           "ESP 未平衡: %#x 期望 %#x", ctx.gpr[CPU_REG_ESP], TEST_ESP_INIT);
}

/* mov [esp], eax 写入后 mov eax, [esp] 读出一致 (内存读写测试) */
TEST(interp_mov_mem_store_load) {
    cpu_context_t ctx;
    static const uint8_t code[] = {
        /* 先存 0x42 到 EAX */
        0xB8, 0x42, 0x00, 0x00, 0x00, /* mov eax, 0x42 */
        /* 存到 [esp] */
        0x89, 0x04, 0x24,             /* mov [esp], eax */
        /* 清零 EAX */
        0x31, 0xC0,                   /* xor eax, eax */
        /* 从 [esp] 读回 */
        0x8B, 0x04, 0x24,             /* mov eax, [esp] */
        0xF4,
    };
    run_code(code, sizeof(code), &ctx);
    ASSERT(ctx.status == CPU_OK, "未正常停止, status=%d", ctx.status);
    ASSERT(ctx.gpr[CPU_REG_EAX] == 0x42,
           "EAX=%#x 期望 0x42 (内存读写不一致)", ctx.gpr[CPU_REG_EAX]);
}
