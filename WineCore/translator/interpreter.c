/* interpreter.c — i386 解释器主循环 (GREEN 阶段)
 *
 * 实现 computed-goto 风格 (此处用 switch) 的指令分发循环。
 * Phase 1 仅实现 hello.exe 与单指令测试所需的指令子集 (~15 条)。
 *
 * 内存模型: flat, guest 地址 = mem_base 偏移 (无分段/分页)
 * 终止条件: hlt (0xF4) / ExitProcess longjmp / #UD longjmp
 *
 * 指令覆盖 (按 opcode):
 *   B8-BF  mov r32, imm32
 *   58-5F  pop r32
 *   50-57  push r32
 *   6A     push imm8 (sign-extended)
 *   68     push imm32
 *   01     add r/m32, r32
 *   29     sub r/m32, r32    (hello.exe 用)
 *   31     xor r/m32, r32
 *   39     cmp r/m32, r32    (hello.exe 用)
 *   85     test r/m32, r32   (hello.exe 用)
 *   83 /0  add r/m32, imm8
 *   83 /4  and r/m32, imm8   (hello.exe 用)
 *   83 /5  sub r/m32, imm8
 *   83 /6  xor r/m32, imm8
 *   83 /7  cmp r/m32, imm8
 *   89     mov r/m32, r32
 *   8B     mov r32, r/m32
 *   8D     lea r32, m        (hello.exe 用)
 *   C7 /0  mov r/m32, imm32 (hello.exe 用)
 *   A1     mov eax, moffs   (hello.exe 读 IAT 槽用)
 *   A3     mov moffs, eax
 *   FF /2  call r/m32       (IAT 调用: mov eax,[iat]; call eax)
 *   FF /6  push r/m32       (hello.exe prologue: push [ecx-4])
 *   EB     jmp rel8
 *   E9     jmp rel32
 *   74     jz rel8
 *   75     jnz rel8          (hello.exe 用)
 *   E8     call rel32
 *   C3     ret
 *   C9     leave
 *   F4     hlt
 *
 * IAT 调用两种模式:
 *   1. mov eax,[iat_slot]; call eax  — FF /2 检测 eax >= WINE_FUNC_ID_BASE
 *   2. call iat_slot_addr (E8 直达)  — 主循环检测 EIP 落入 IAT 区间
 */
#include "wine/cpu.h"
#include "wine/wine.h"
#include <string.h>

/* ---- 对齐安全的多字节读取 ---- */
static uint32_t read_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static int32_t read_s32(const uint8_t *p) {
    int32_t v; memcpy(&v, p, 4); return v;
}

/* ---- guest 内存访问 ---- */
static uint8_t *gp(cpu_context_t *ctx, uint32_t addr) {
    return ctx->mem_base + addr;
}

static uint32_t mem_r32(cpu_context_t *ctx, uint32_t addr) {
    return read_u32(gp(ctx, addr));
}

static void mem_w32(cpu_context_t *ctx, uint32_t addr, uint32_t val) {
    memcpy(gp(ctx, addr), &val, 4);
}

/* ---- 栈操作 ---- */
static void push32(cpu_context_t *ctx, uint32_t val) {
    ctx->gpr[CPU_REG_ESP] -= 4;
    mem_w32(ctx, ctx->gpr[CPU_REG_ESP], val);
}

static uint32_t pop32(cpu_context_t *ctx) {
    uint32_t v = mem_r32(ctx, ctx->gpr[CPU_REG_ESP]);
    ctx->gpr[CPU_REG_ESP] += 4;
    return v;
}

/* ---- ModRM/SIB 解码 ---- */
typedef struct {
    uint8_t  mod;
    uint8_t  reg;
    uint8_t  rm;
    uint32_t ea;   /* 有效地址 (mod != 3 时) */
} modrm_t;

static modrm_t decode_modrm(cpu_context_t *ctx, uint32_t *eip) {
    uint8_t b = *gp(ctx, (*eip)++);
    modrm_t m;
    m.mod = (b >> 6) & 3;
    m.reg = (b >> 3) & 7;
    m.rm  = b & 7;
    m.ea  = 0;

    if (m.mod == 3) return m; /* 寄存器直接 */

    /* rm == 4: SIB 字节跟随 */
    if (m.rm == 4) {
        uint8_t sib = *gp(ctx, (*eip)++);
        uint32_t scale = 1u << ((sib >> 6) & 3);
        uint8_t  index = (sib >> 3) & 7;
        uint8_t  base  = sib & 7;
        if (base == 5 && m.mod == 0) {
            /* mod=0, base=5: 32 位 disp 作为基址 */
            m.ea = read_u32(gp(ctx, *eip)); *eip += 4;
        } else {
            m.ea = ctx->gpr[base];
        }
        if (index != 4) m.ea += ctx->gpr[index] * scale;
    } else if (m.rm == 5 && m.mod == 0) {
        /* mod=0, rm=5: 绝对 disp32 */
        m.ea = read_u32(gp(ctx, *eip)); *eip += 4;
    } else {
        m.ea = ctx->gpr[m.rm];
    }

    if (m.mod == 1) {
        int8_t d = (int8_t)*gp(ctx, *eip); (*eip)++;
        m.ea += (int32_t)d;
    } else if (m.mod == 2) {
        int32_t d = read_s32(gp(ctx, *eip)); *eip += 4;
        m.ea += d;
    }
    return m;
}

static uint32_t rm_r(cpu_context_t *ctx, modrm_t *m) {
    return (m->mod == 3) ? ctx->gpr[m->rm] : mem_r32(ctx, m->ea);
}

static void rm_w(cpu_context_t *ctx, modrm_t *m, uint32_t val) {
    if (m->mod == 3) ctx->gpr[m->rm] = val;
    else mem_w32(ctx, m->ea, val);
}

/* ---- 标志位计算 ---- */
static void set_zf(cpu_context_t *c, uint32_t r) {
    if (r == 0) c->eflags |=  (1u << CPU_FLAG_ZF);
    else        c->eflags &= ~(1u << CPU_FLAG_ZF);
}
static void set_sf(cpu_context_t *c, uint32_t r) {
    if (r & 0x80000000u) c->eflags |=  (1u << CPU_FLAG_SF);
    else                 c->eflags &= ~(1u << CPU_FLAG_SF);
}
static void set_flag(cpu_context_t *c, int bit, int on) {
    if (on) c->eflags |=  (1u << bit);
    else    c->eflags &= ~(1u << bit);
}

/* ADD 标志: CF=无符号溢出, OF=有符号溢出, ZF/SF 按结果 */
static void flags_add(cpu_context_t *c, uint32_t a, uint32_t b, uint32_t r) {
    set_zf(c, r);
    set_sf(c, r);
    set_flag(c, CPU_FLAG_CF, r < a);
    int sa = (int32_t)a < 0, sb = (int32_t)b < 0, sr = (int32_t)r < 0;
    set_flag(c, CPU_FLAG_OF, (sa == sb) && (sa != sr));
}

/* SUB 标志: CF=a<b (借位), OF=有符号溢出 */
static void flags_sub(cpu_context_t *c, uint32_t a, uint32_t b, uint32_t r) {
    set_zf(c, r);
    set_sf(c, r);
    set_flag(c, CPU_FLAG_CF, a < b);
    int sa = (int32_t)a < 0, sb = (int32_t)b < 0, sr = (int32_t)r < 0;
    set_flag(c, CPU_FLAG_OF, (sa != sb) && (sa != sr));
}

/* 逻辑运算标志: CF=0, OF=0, ZF/SF 按结果 */
static void flags_logic(cpu_context_t *c, uint32_t r) {
    set_zf(c, r);
    set_sf(c, r);
    set_flag(c, CPU_FLAG_CF, 0);
    set_flag(c, CPU_FLAG_OF, 0);
}

/* ---- ExitProcess / #UD ---- */
void cpu_exit_process(cpu_context_t *ctx, uint32_t code) {
    ctx->exit_code = code;
    ctx->status = CPU_EXIT_PROCESS;
    longjmp(ctx->exit_jmp, 1);
}

void cpu_raise_ud(cpu_context_t *ctx) {
    ctx->exit_code = 0xC000001D;
    ctx->status = CPU_ERROR_UD;
    longjmp(ctx->exit_jmp, 1);
}

/* ---- 主循环 ---- */
cpu_status_t cpu_run(cpu_context_t *ctx, uint32_t start_eip) {
    ctx->eip = start_eip;
    if (setjmp(ctx->exit_jmp) != 0) {
        return ctx->status; /* ExitProcess / #UD longjmp 回来 */
    }

    for (;;) {
        if (ctx->eip >= ctx->mem_size) cpu_raise_ud(ctx);

        /* IAT 槽直接作为调用目标 (E8 call iat_addr 落入 IAT 区间)
         * 此时返回地址已由 call 压栈, 读取 IAT 槽中的函数 id 分发到 thunk,
         * 执行后模拟 ret 弹出返回地址。 */
        if (ctx->current_image && ctx->current_image->iat_base
            && ctx->eip >= ctx->current_image->iat_base
            && ctx->eip < ctx->current_image->iat_base + ctx->current_image->iat_size) {
            uint32_t func_id = mem_r32(ctx, ctx->eip);
            wine_thunk_t thunk = (wine_thunk_t)wine_func_get(func_id);
            if (!thunk) cpu_raise_ud(ctx);
            thunk(ctx);
            ctx->eip = pop32(ctx); /* 模拟 ret */
            continue;
        }

        uint8_t op = *gp(ctx, ctx->eip++);

        /* mov r32, imm32 (B8-BF) */
        if (op >= 0xB8 && op <= 0xBF) {
            uint8_t reg = op - 0xB8;
            ctx->gpr[reg] = read_u32(gp(ctx, ctx->eip));
            ctx->eip += 4;
            continue;
        }
        /* pop r32 (58-5F) */
        if (op >= 0x58 && op <= 0x5F) {
            ctx->gpr[op - 0x58] = pop32(ctx);
            continue;
        }
        /* push r32 (50-57) — hello.exe prologue 用 */
        if (op >= 0x50 && op <= 0x57) {
            push32(ctx, ctx->gpr[op - 0x50]);
            continue;
        }

        switch (op) {
        case 0x6A: { /* push imm8 (sign-extended) */
            int8_t v = (int8_t)*gp(ctx, ctx->eip++);
            push32(ctx, (uint32_t)(int32_t)v);
            break;
        }
        case 0x68: { /* push imm32 */
            uint32_t v = read_u32(gp(ctx, ctx->eip)); ctx->eip += 4;
            push32(ctx, v);
            break;
        }

        /* 算术/逻辑: r/m32, r32 */
        case 0x01: { /* add */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t a = rm_r(ctx, &m), b = ctx->gpr[m.reg], r = a + b;
            rm_w(ctx, &m, r); flags_add(ctx, a, b, r);
            break;
        }
        case 0x29: { /* sub */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t a = rm_r(ctx, &m), b = ctx->gpr[m.reg], r = a - b;
            rm_w(ctx, &m, r); flags_sub(ctx, a, b, r);
            break;
        }
        case 0x31: { /* xor */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t r = rm_r(ctx, &m) ^ ctx->gpr[m.reg];
            rm_w(ctx, &m, r); flags_logic(ctx, r);
            break;
        }
        case 0x21: { /* and */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t r = rm_r(ctx, &m) & ctx->gpr[m.reg];
            rm_w(ctx, &m, r); flags_logic(ctx, r);
            break;
        }
        case 0x09: { /* or */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t r = rm_r(ctx, &m) | ctx->gpr[m.reg];
            rm_w(ctx, &m, r); flags_logic(ctx, r);
            break;
        }
        case 0x39: { /* cmp r/m32, r32 */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t a = rm_r(ctx, &m), b = ctx->gpr[m.reg];
            flags_sub(ctx, a, b, a - b);
            break;
        }
        case 0x85: { /* test r/m32, r32 */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            flags_logic(ctx, rm_r(ctx, &m) & ctx->gpr[m.reg]);
            break;
        }

        /* group 1: 83 /r r/m32, imm8 (sign-extended) */
        case 0x83: {
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            int8_t imm = (int8_t)*gp(ctx, ctx->eip++);
            uint32_t a = rm_r(ctx, &m), b = (uint32_t)(int32_t)imm, r;
            switch (m.reg) {
            case 0: r=a+b; flags_add(ctx,a,b,r); rm_w(ctx,&m,r); break; /* add */
            case 1: r=a|b; flags_logic(ctx,r); rm_w(ctx,&m,r); break;  /* or  */
            case 4: r=a&b; flags_logic(ctx,r); rm_w(ctx,&m,r); break;  /* and */
            case 5: r=a-b; flags_sub(ctx,a,b,r); rm_w(ctx,&m,r); break;/* sub */
            case 6: r=a^b; flags_logic(ctx,r); rm_w(ctx,&m,r); break;  /* xor */
            case 7: r=a-b; flags_sub(ctx,a,b,r); break;               /* cmp */
            default: cpu_raise_ud(ctx);
            }
            break;
        }

        /* 数据传送 */
        case 0x89: { /* mov r/m32, r32 */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            rm_w(ctx, &m, ctx->gpr[m.reg]);
            break;
        }
        case 0x8B: { /* mov r32, r/m32 */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            ctx->gpr[m.reg] = rm_r(ctx, &m);
            break;
        }
        case 0x8D: { /* lea r32, m */
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            ctx->gpr[m.reg] = m.ea;
            break;
        }

        /* mov r/m32, imm32 (C7 /0) — hello.exe 局部变量初始化用 */
        case 0xC7: {
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            uint32_t imm = read_u32(gp(ctx, ctx->eip)); ctx->eip += 4;
            if (m.reg != 0) cpu_raise_ud(ctx); /* Phase 1 仅 /0 */
            rm_w(ctx, &m, imm);
            break;
        }

        /* mov eax, moffs (A1) / mov moffs, eax (A3) — hello.exe 读 IAT 槽用 */
        case 0xA1: { /* mov eax, [disp32] */
            uint32_t disp = read_u32(gp(ctx, ctx->eip)); ctx->eip += 4;
            ctx->gpr[CPU_REG_EAX] = mem_r32(ctx, disp);
            break;
        }
        case 0xA3: { /* mov [disp32], eax */
            uint32_t disp = read_u32(gp(ctx, ctx->eip)); ctx->eip += 4;
            mem_w32(ctx, disp, ctx->gpr[CPU_REG_EAX]);
            break;
        }

        /* FF 组: /2 call r/m32, /6 push r/m32
         *   call r/m32: 若目标 >= WINE_FUNC_ID_BASE 则为 IAT 函数 id, 直接分发 thunk;
         *               否则按普通 call 处理 (压返回地址, 跳转) */
        case 0xFF: {
            modrm_t m = decode_modrm(ctx, &ctx->eip);
            switch (m.reg) {
            case 2: { /* call r/m32 */
                uint32_t target = rm_r(ctx, &m);
                if (target >= WINE_FUNC_ID_BASE) {
                    /* IAT 调用 (mov eax,[iat]; call eax): eax 存函数 id */
                    wine_thunk_t thunk = (wine_thunk_t)wine_func_get(target);
                    if (!thunk) cpu_raise_ud(ctx);
                    push32(ctx, ctx->eip);
                    thunk(ctx);
                    ctx->eip = pop32(ctx); /* 模拟 ret */
                } else {
                    push32(ctx, ctx->eip);
                    ctx->eip = target;
                }
                break;
            }
            case 6: /* push r/m32 — hello.exe prologue: push [ecx-4] */
                push32(ctx, rm_r(ctx, &m));
                break;
            default:
                cpu_raise_ud(ctx);
            }
            break;
        }

        /* 控制流 */
        case 0xEB: { /* jmp rel8 */
            int8_t rel = (int8_t)*gp(ctx, ctx->eip++);
            ctx->eip += (int32_t)rel;
            break;
        }
        case 0xE9: { /* jmp rel32 */
            int32_t rel = read_s32(gp(ctx, ctx->eip)); ctx->eip += 4;
            ctx->eip += rel;
            break;
        }
        case 0x74: { /* jz rel8 */
            int8_t rel = (int8_t)*gp(ctx, ctx->eip++);
            if (ctx->eflags & (1u << CPU_FLAG_ZF)) ctx->eip += (int32_t)rel;
            break;
        }
        case 0x75: { /* jnz rel8 */
            int8_t rel = (int8_t)*gp(ctx, ctx->eip++);
            if (!(ctx->eflags & (1u << CPU_FLAG_ZF))) ctx->eip += (int32_t)rel;
            break;
        }
        case 0xE8: { /* call rel32 */
            int32_t rel = read_s32(gp(ctx, ctx->eip)); ctx->eip += 4;
            push32(ctx, ctx->eip);
            ctx->eip += rel;
            break;
        }
        case 0xC3: { /* ret */
            ctx->eip = pop32(ctx);
            break;
        }
        case 0xC9: { /* leave = mov esp,ebp; pop ebp */
            ctx->gpr[CPU_REG_ESP] = ctx->gpr[CPU_REG_EBP];
            ctx->gpr[CPU_REG_EBP] = pop32(ctx);
            break;
        }

        case 0xF4: /* hlt — 正常停止 */
            ctx->status = CPU_OK;
            return CPU_OK;

        default:
            cpu_raise_ud(ctx);
        }
    }
}
