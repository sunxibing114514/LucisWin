/* block_cache.c — Phase 2.4 基本块翻译缓存 (GREEN)
 *
 * 预解码 i386 基本块为 uop 序列, 重复执行时跳过逐字节解码。
 * 仅翻译 fib.exe 热循环用到的指令子集 (mov/alu/inc/dec); 其余一律
 * UOP_FALLBACK 降级到既有 switch 解释器, 保证语义不变。
 *
 * 安全保证: 缓存只加速, 不改语义。flag helper / modrm 解码与
 * interpreter.c 保持一致 (本文件自带副本, 单测 cache_disabled_vs_enabled
 * 对比启用前后结果一致作为守卫)。详见
 * docs/superpowers/specs/2026-07-23-luciswin-phase2.4-blockcache-design.md
 */
#include "block_cache.h"
#include <string.h>

/* ============================================================
 * uop 定义
 * ============================================================ */
typedef enum {
    UOP_MOV_REG_IMM,
    UOP_MOV_REG_REG,
    UOP_MOV_MEM_REG,
    UOP_MOV_REG_MEM,
    UOP_ALU_RR,
    UOP_ALU_RM,
    UOP_ALU_IMM,      /* 0x83 imm8 与 0x81 imm32 统一, imm 已扩展为 uint32 */
    UOP_INC,
    UOP_DEC,
    UOP_FALLBACK,
} uop_op_t;

typedef enum {
    ALU_ADD, ALU_SUB, ALU_AND, ALU_OR, ALU_XOR, ALU_CMP, ALU_TEST
} alu_op_t;

/* 预解码的内存操作数 form (运行时 EA = gpr[base] + gpr[index]*scale + disp) */
typedef struct {
    uint8_t  base;    /* 0-7, 或 0xFF=无 */
    uint8_t  index;   /* 0-7, 或 0xFF=无 (SIB index==4 表示无变址) */
    uint8_t  scale;   /* 1/2/4/8 */
    uint32_t disp;    /* 已按 mod 符号扩展为 uint32 */
} mem_form_t;

typedef struct {
    uop_op_t  op;
    alu_op_t  alu;       /* 仅 ALU_* 用 */
    uint8_t   dst;       /* 目的寄存器下标 0-7 */
    uint8_t   src;       /* 源寄存器下标 0-7 */
    uint32_t  imm;       /* 立即数 (已扩展为 32 位) */
    mem_form_t mem;      /* 仅 *_MEM 用 */
    uint32_t  next_eip;  /* 本 uop 执行后 ctx->eip 应推进到的位置 */
    uint32_t  fallback_eip; /* 仅 UOP_FALLBACK 用: 慢路径起点 */
} uop_t;

#define UOP_BLOCK_MAX_INSNS 32
typedef struct {
    uint32_t start_eip;
    int      n_uops;
    int      valid;
    uop_t    uops[UOP_BLOCK_MAX_INSNS];
} block_t;

#define BLOCK_CACHE_SIZE 256
static block_t g_cache[BLOCK_CACHE_SIZE];
static uint64_t g_hits   = 0;
static uint64_t g_misses = 0;
static int      g_enabled = 1;

/* ============================================================
 * flag helper (与 interpreter.c 保持一致)
 * ============================================================ */
enum {
    CPU_FLAG_CF_ = 0, CPU_FLAG_PF_ = 2, CPU_FLAG_AF_ = 4,
    CPU_FLAG_ZF_ = 6, CPU_FLAG_SF_ = 7, CPU_FLAG_OF_ = 11,
};

static void bc_set_zf(cpu_context_t *c, uint32_t r) {
    if (r == 0) c->eflags |=  (1u << CPU_FLAG_ZF_);
    else        c->eflags &= ~(1u << CPU_FLAG_ZF_);
}
static void bc_set_sf(cpu_context_t *c, uint32_t r) {
    if (r & 0x80000000u) c->eflags |=  (1u << CPU_FLAG_SF_);
    else                 c->eflags &= ~(1u << CPU_FLAG_SF_);
}
static void bc_set_flag(cpu_context_t *c, int bit, int on) {
    if (on) c->eflags |=  (1u << bit);
    else    c->eflags &= ~(1u << bit);
}
static void bc_flags_add(cpu_context_t *c, uint32_t a, uint32_t b, uint32_t r) {
    bc_set_zf(c, r); bc_set_sf(c, r);
    bc_set_flag(c, CPU_FLAG_CF_, r < a);
    int sa = (int32_t)a < 0, sb = (int32_t)b < 0, sr = (int32_t)r < 0;
    bc_set_flag(c, CPU_FLAG_OF_, (sa == sb) && (sa != sr));
}
static void bc_flags_sub(cpu_context_t *c, uint32_t a, uint32_t b, uint32_t r) {
    bc_set_zf(c, r); bc_set_sf(c, r);
    bc_set_flag(c, CPU_FLAG_CF_, a < b);
    int sa = (int32_t)a < 0, sb = (int32_t)b < 0, sr = (int32_t)r < 0;
    bc_set_flag(c, CPU_FLAG_OF_, (sa != sb) && (sa != sr));
}
static void bc_flags_logic(cpu_context_t *c, uint32_t r) {
    bc_set_zf(c, r); bc_set_sf(c, r);
    bc_set_flag(c, CPU_FLAG_CF_, 0);
    bc_set_flag(c, CPU_FLAG_OF_, 0);
}

/* ============================================================
 * guest 内存访问 (与 interpreter.c 一致, unaligned-safe)
 * ============================================================ */
static uint8_t *bc_gp(cpu_context_t *ctx, uint32_t addr) {
    return ctx->mem_base + addr;
}
static uint32_t bc_read_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static uint32_t bc_mem_r32(cpu_context_t *ctx, uint32_t addr) {
    return bc_read_u32(bc_gp(ctx, addr));
}
static void bc_mem_w32(cpu_context_t *ctx, uint32_t addr, uint32_t val) {
    memcpy(bc_gp(ctx, addr), &val, 4);
}

/* ============================================================
 * ModRM 解码 (与 interpreter.c decode_modrm 同逻辑, 输出 mem_form_t)
 *
 * mem_form 存运行时重算 EA 所需的 base/index/scale/disp:
 *   ea = gpr[base] + gpr[index]*scale + disp   (base/index==0xFF 表示无)
 * 翻译时用当前寄存器快照算出 ea, 再减回寄存器部分, 剩下纯 disp。
 * 运行时寄存器已变, bc_compute_ea 用新的寄存器值重算, 结果正确。
 * ============================================================ */
static void bc_decode_modrm(cpu_context_t *ctx, uint32_t *eip,
                            uint8_t *out_mod, uint8_t *out_reg, uint8_t *out_rm,
                            mem_form_t *out_mem) {
    uint8_t b = *bc_gp(ctx, (*eip)++);
    uint8_t mod = (b >> 6) & 3;
    uint8_t reg = (b >> 3) & 7;
    uint8_t rm  = b & 7;
    *out_mod = mod; *out_reg = reg; *out_rm = rm;

    mem_form_t mf = { 0xFF, 0xFF, 1, 0 };
    uint32_t ea = 0;

    if (mod != 3) {
        if (rm == 4) {  /* SIB */
            uint8_t sib = *bc_gp(ctx, (*eip)++);
            uint32_t scale = 1u << ((sib >> 6) & 3);
            uint8_t index = (sib >> 3) & 7;
            uint8_t base  = sib & 7;
            if (base == 5 && mod == 0) {
                /* mod=0, base=5: 绝对 disp32 作基址, 无寄存器 */
                ea = bc_read_u32(bc_gp(ctx, *eip)); *eip += 4;
            } else {
                ea = ctx->gpr[base];
                mf.base = base;
            }
            if (index != 4) {  /* index==4 表示无变址 */
                ea += ctx->gpr[index] * scale;
                mf.index = index; mf.scale = (uint8_t)scale;
            }
        } else if (rm == 5 && mod == 0) {
            /* mod=0, rm=5: 绝对 disp32 */
            ea = bc_read_u32(bc_gp(ctx, *eip)); *eip += 4;
        } else {
            ea = ctx->gpr[rm];
            mf.base = rm;
        }
        if (mod == 1) {
            int8_t d = (int8_t)*bc_gp(ctx, *eip); (*eip)++;
            ea += (int32_t)d;
        } else if (mod == 2) {
            int32_t d = (int32_t)bc_read_u32(bc_gp(ctx, *eip)); *eip += 4;
            ea += d;
        }
        /* disp = ea - 寄存器贡献, 只留纯位移 (绝对 disp32 时 base=index=0xFF, disp=ea) */
        mf.disp = ea
                - ((mf.base  != 0xFF) ? ctx->gpr[mf.base] : 0)
                - ((mf.index != 0xFF) ? ctx->gpr[mf.index] * mf.scale : 0);
    }
    *out_mem = mf;
}

/* ============================================================
 * 翻译: 从 start_eip 起逐条译码, 填 block->uops
 * 遇控制流/不支持指令 -> 该指令作 UOP_FALLBACK 结束块
 * ============================================================ */
static void block_translate(cpu_context_t *ctx, uint32_t start_eip, block_t *b) {
    b->start_eip = start_eip;
    b->n_uops = 0;
    b->valid = 1;

    uint32_t eip = start_eip;
    while (b->n_uops < UOP_BLOCK_MAX_INSNS) {
        if (eip >= ctx->mem_size) {
            /* 越界: fallback 让主循环 #UD */
            uop_t *u = &b->uops[b->n_uops++];
            u->op = UOP_FALLBACK;
            u->fallback_eip = eip;
            return;
        }
        uint32_t insn_start = eip;
        uint8_t op = *bc_gp(ctx, eip++);

        uop_t *u = &b->uops[b->n_uops];
        memset(u, 0, sizeof(*u));

        /* mov r32, imm32 (B8-BF) */
        if (op >= 0xB8 && op <= 0xBF) {
            uint32_t imm = bc_read_u32(bc_gp(ctx, eip)); eip += 4;
            u->op = UOP_MOV_REG_IMM;
            u->dst = op - 0xB8;
            u->imm = imm;
            u->next_eip = eip;
            b->n_uops++;
            continue;
        }
        /* inc r32 (40-47) */
        if (op >= 0x40 && op <= 0x47) {
            u->op = UOP_INC;
            u->dst = op - 0x40;
            u->next_eip = eip;
            b->n_uops++;
            continue;
        }
        /* dec r32 (48-4F) */
        if (op >= 0x48 && op <= 0x4F) {
            u->op = UOP_DEC;
            u->dst = op - 0x48;
            u->next_eip = eip;
            b->n_uops++;
            continue;
        }
        /* ALU r/m32, r32: 01=add 29=sub 21=and 09=or 31=xor 39=cmp 85=test */
        if (op == 0x01 || op == 0x29 || op == 0x21 || op == 0x09 ||
            op == 0x31 || op == 0x39 || op == 0x85) {
            alu_op_t alu;
            switch (op) {
            case 0x01: alu = ALU_ADD; break;
            case 0x29: alu = ALU_SUB; break;
            case 0x21: alu = ALU_AND; break;
            case 0x09: alu = ALU_OR;  break;
            case 0x31: alu = ALU_XOR; break;
            case 0x39: alu = ALU_CMP; break;
            case 0x85: alu = ALU_TEST; break;
            default: alu = ALU_ADD; /* unreachable */
            }
            uint8_t mod, reg, rm; mem_form_t mf;
            bc_decode_modrm(ctx, &eip, &mod, &reg, &rm, &mf);
            u->alu = alu;
            u->src = reg;
            u->next_eip = eip;
            if (mod == 3) {
                u->op = UOP_ALU_RR;
                u->dst = rm;  /* r/m 是目的 (除 cmp/test) */
            } else {
                u->op = UOP_ALU_RM;
                u->mem = mf;
                u->dst = 0; /* rm 是内存, 不用 dst */
            }
            b->n_uops++;
            continue;
        }
        /* mov r/m32, r32 (89) */
        if (op == 0x89) {
            uint8_t mod, reg, rm; mem_form_t mf;
            bc_decode_modrm(ctx, &eip, &mod, &reg, &rm, &mf);
            u->src = reg;
            u->next_eip = eip;
            if (mod == 3) {
                u->op = UOP_MOV_REG_REG;
                u->dst = rm;  /* r/m 是目的 */
            } else {
                u->op = UOP_MOV_MEM_REG;
                u->mem = mf;
            }
            b->n_uops++;
            continue;
        }
        /* mov r32, r/m32 (8B) */
        if (op == 0x8B) {
            uint8_t mod, reg, rm; mem_form_t mf;
            bc_decode_modrm(ctx, &eip, &mod, &reg, &rm, &mf);
            u->dst = reg;
            u->next_eip = eip;
            if (mod == 3) {
                u->op = UOP_MOV_REG_REG;
                u->src = rm;
            } else {
                u->op = UOP_MOV_REG_MEM;
                u->mem = mf;
            }
            b->n_uops++;
            continue;
        }
        /* group 1: 83 /r r/m32, imm8 (sign-extended) — 仅 mod=3 (寄存器) */
        if (op == 0x83) {
            uint8_t mod, reg, rm; mem_form_t mf;
            bc_decode_modrm(ctx, &eip, &mod, &reg, &rm, &mf);
            if (mod == 3 && (reg == 0 || reg == 4 || reg == 5 || reg == 6 || reg == 7)) {
                int8_t imm = (int8_t)*bc_gp(ctx, eip++);
                alu_op_t alu;
                switch (reg) {
                case 0: alu = ALU_ADD; break;
                case 4: alu = ALU_AND; break;
                case 5: alu = ALU_SUB; break;
                case 6: alu = ALU_XOR; break;
                case 7: alu = ALU_CMP; break;
                default: alu = ALU_ADD; /* unreachable */
                }
                u->op = UOP_ALU_IMM;
                u->alu = alu;
                u->dst = rm;
                u->imm = (uint32_t)(int32_t)imm;
                u->next_eip = eip;
                b->n_uops++;
                continue;
            }
            /* mod!=3 或未支持的 /r: fallback。
             * 但 eip 已被 decode_modrm 推进, 需回退到 insn_start */
            u->op = UOP_FALLBACK;
            u->fallback_eip = insn_start;
            b->n_uops++;
            return;
        }
        /* group 1: 81 /r r/m32, imm32 — 仅 mod=3 */
        if (op == 0x81) {
            uint8_t mod, reg, rm; mem_form_t mf;
            bc_decode_modrm(ctx, &eip, &mod, &reg, &rm, &mf);
            if (mod == 3 && (reg == 0 || reg == 4 || reg == 5 || reg == 6 || reg == 7)) {
                uint32_t imm = bc_read_u32(bc_gp(ctx, eip)); eip += 4;
                alu_op_t alu;
                switch (reg) {
                case 0: alu = ALU_ADD; break;
                case 4: alu = ALU_AND; break;
                case 5: alu = ALU_SUB; break;
                case 6: alu = ALU_XOR; break;
                case 7: alu = ALU_CMP; break;
                default: alu = ALU_ADD;
                }
                u->op = UOP_ALU_IMM;
                u->alu = alu;
                u->dst = rm;
                u->imm = imm;
                u->next_eip = eip;
                b->n_uops++;
                continue;
            }
            u->op = UOP_FALLBACK;
            u->fallback_eip = insn_start;
            b->n_uops++;
            return;
        }

        /* 其余 opcode (含所有控制流) -> fallback */
        u->op = UOP_FALLBACK;
        u->fallback_eip = insn_start;
        b->n_uops++;
        return;
    }
    /* 块满结束: 最后一条 uop 的 next_eip 指向下一条指令,
     * 主循环 continue 后会重新查缓存翻译新块。 */
}

/* ============================================================
 * 执行: 跑 block->uops, 遇 FALLBACK 设 eip 后返回
 * ============================================================ */
static uint32_t bc_compute_ea(cpu_context_t *ctx, const mem_form_t *mf) {
    uint32_t ea = mf->disp;
    if (mf->base != 0xFF)  ea += ctx->gpr[mf->base];
    if (mf->index != 0xFF) ea += ctx->gpr[mf->index] * mf->scale;
    return ea;
}

static void bc_alu(cpu_context_t *ctx, alu_op_t alu,
                   uint32_t a, uint32_t b, uint32_t *out_r) {
    uint32_t r;
    switch (alu) {
    case ALU_ADD:  r = a + b; bc_flags_add(ctx, a, b, r);    *out_r = r; break;
    case ALU_SUB:  r = a - b; bc_flags_sub(ctx, a, b, r);    *out_r = r; break;
    case ALU_AND:  r = a & b; bc_flags_logic(ctx, r);        *out_r = r; break;
    case ALU_OR:   r = a | b; bc_flags_logic(ctx, r);        *out_r = r; break;
    case ALU_XOR:  r = a ^ b; bc_flags_logic(ctx, r);        *out_r = r; break;
    case ALU_CMP:  r = a - b; bc_flags_sub(ctx, a, b, r);    *out_r = a; break; /* 不写回 */
    case ALU_TEST: r = a & b; bc_flags_logic(ctx, r);        *out_r = a; break; /* 不写回 */
    }
}

static void block_exec(cpu_context_t *ctx, block_t *b) {
    for (int i = 0; i < b->n_uops; i++) {
        const uop_t *u = &b->uops[i];
        switch (u->op) {
        case UOP_MOV_REG_IMM:
            ctx->gpr[u->dst] = u->imm;
            ctx->eip = u->next_eip;
            break;
        case UOP_MOV_REG_REG:
            ctx->gpr[u->dst] = ctx->gpr[u->src];
            ctx->eip = u->next_eip;
            break;
        case UOP_MOV_MEM_REG:
            bc_mem_w32(ctx, bc_compute_ea(ctx, &u->mem), ctx->gpr[u->src]);
            ctx->eip = u->next_eip;
            break;
        case UOP_MOV_REG_MEM: {
            uint32_t v = bc_mem_r32(ctx, bc_compute_ea(ctx, &u->mem));
            ctx->gpr[u->dst] = v;
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_ALU_RR: {
            uint32_t a = ctx->gpr[u->dst], b = ctx->gpr[u->src], r;
            bc_alu(ctx, u->alu, a, b, &r);
            ctx->gpr[u->dst] = r;
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_ALU_RM: {
            uint32_t ea = bc_compute_ea(ctx, &u->mem);
            uint32_t a = bc_mem_r32(ctx, ea), b = ctx->gpr[u->src], r;
            bc_alu(ctx, u->alu, a, b, &r);
            bc_mem_w32(ctx, ea, r);
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_ALU_IMM: {
            uint32_t a = ctx->gpr[u->dst], b = u->imm, r;
            bc_alu(ctx, u->alu, a, b, &r);
            ctx->gpr[u->dst] = r;
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_INC: {
            uint32_t a = ctx->gpr[u->dst], r = a + 1;
            ctx->gpr[u->dst] = r;
            bc_set_zf(ctx, r); bc_set_sf(ctx, r);
            bc_set_flag(ctx, CPU_FLAG_OF_, (a & 0x80000000u) && !(r & 0x80000000u));
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_DEC: {
            uint32_t a = ctx->gpr[u->dst], r = a - 1;
            ctx->gpr[u->dst] = r;
            bc_set_zf(ctx, r); bc_set_sf(ctx, r);
            bc_set_flag(ctx, CPU_FLAG_OF_, (!(a & 0x80000000u)) && (r & 0x80000000u));
            ctx->eip = u->next_eip;
            break;
        }
        case UOP_FALLBACK:
            ctx->eip = u->fallback_eip;
            return;
        }
    }
}

/* ============================================================
 * 公开接口
 * ============================================================ */
int block_cache_try_exec(cpu_context_t *ctx, uint32_t eip) {
    if (!g_enabled) return 0;
    uint32_t slot = eip % BLOCK_CACHE_SIZE;
    block_t *b = &g_cache[slot];
    if (b->valid && b->start_eip == eip) {
        g_hits++;
        block_exec(ctx, b);
        return 1;
    }
    g_misses++;
    memset(b, 0, sizeof(*b));
    block_translate(ctx, eip, b);
    /* 首条就是 fallback (控制流/不支持指令): 不缓存不执行,
     * 让主循环走 switch 处理该指令。
     * 否则命中后 block_exec 会把 eip 设回 fallback_eip (== 入参 eip),
     * 主循环 continue 再进来 -> 又命中 -> 又设回同点 -> 死循环。
     * 标记 valid=0 让下次仍走 miss -> switch 路径。 */
    if (b->n_uops == 1 && b->uops[0].op == UOP_FALLBACK) {
        b->valid = 0;
        return 0;
    }
    block_exec(ctx, b);
    return 1;
}

uint64_t block_cache_hits(void)   { return g_hits; }
uint64_t block_cache_misses(void) { return g_misses; }

void block_cache_reset(void) {
    g_hits = 0;
    g_misses = 0;
    memset(g_cache, 0, sizeof(g_cache));
}

void block_cache_disable(void) { g_enabled = 0; }
void block_cache_enable(void)  { g_enabled = 1; }
int  block_cache_enabled(void) { return g_enabled; }
