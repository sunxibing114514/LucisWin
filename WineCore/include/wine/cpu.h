/* cpu.h — CPU 上下文与 i386 解释器入口
 *
 * 功能: 定义 32 位 i386 CPU 上下文结构, 暴露 cpu_run 解释执行入口。
 *       Phase 1 采用 flat memory model, 所有 guest 地址为 mem_base 的偏移。
 *
 * 设计要点:
 *   - gpr[8] 按 ModRM reg 编码顺序 (EAX=0 ... EDI=7), 解释器直接用下标索引
 *   - mem_base: 宿主内存基址, guest 虚拟地址 = mem_base + offset
 *     测试时指向 code+stack 共享缓冲区; 真实 PE 时指向加载后的镜像 (含栈)
 *   - current_image: 用于判断 call 目标是否落在 IAT 区间
 *     (IAT 槽存 wine_func_register 返回的 32 位 id, 非函数指针)
 *   - exit_jmp: ExitProcess 与 #UD 通过 longjmp 跳回 cpu_run
 *
 * Phase 1 范围:
 *   - 仅 i386 32 位保护模式 flat 段
 *   - 不模拟分段/分页/中断/异常 (仅 #UD 走 longjmp)
 *   - 不实现 FPU/SSE (字段仅占位)
 */
#ifndef WINE_CPU_H
#define WINE_CPU_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#include "wine/pe_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 通用寄存器索引 (按 ModRM reg 字段编码顺序) */
enum {
    CPU_REG_EAX = 0,
    CPU_REG_ECX = 1,
    CPU_REG_EDX = 2,
    CPU_REG_EBX = 3,
    CPU_REG_ESP = 4,
    CPU_REG_EBP = 5,
    CPU_REG_ESI = 6,
    CPU_REG_EDI = 7,
};

/* EFLAGS 位位置 (仅 Phase 1 用到的标志) */
enum {
    CPU_FLAG_CF = 0,   /* bit 0:  Carry */
    CPU_FLAG_PF = 2,   /* bit 2:  Parity */
    CPU_FLAG_AF = 4,   /* bit 4:  Auxiliary carry */
    CPU_FLAG_ZF = 6,   /* bit 6:  Zero */
    CPU_FLAG_SF = 7,   /* bit 7:  Sign */
    CPU_FLAG_OF = 11,  /* bit 11: Overflow */
};

/* cpu_run 执行结果状态 */
typedef enum {
    CPU_OK = 0,            /* 遇到 hlt (0xF4), 正常停止 */
    CPU_EXIT_PROCESS = 1,  /* ExitProcess 被调用, exit_code 已写入 */
    CPU_ERROR_UD = 2,      /* 未实现指令 (#UD), exit_code = 0xC000001D */
} cpu_status_t;

/* CPU 上下文: 一个线程的完整执行状态 */
typedef struct cpu_context {
    uint32_t gpr[8];       /* EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI */
    uint32_t eip;          /* 指令指针 (guest 虚拟地址, 即 mem_base 偏移) */
    uint32_t eflags;       /* 标志寄存器 */
    uint16_t seg[6];       /* CS,DS,ES,FS,GS,SS — Phase 1 全 0 (flat) */
    uint32_t seg_base[6];  /* 段基址, Phase 1 全 0 */
    uint8_t  fpu_st[8][10];/* 80-bit x87 寄存器栈, Phase 1 仅占位 */
    uint8_t *mem_base;     /* guest 内存基址 (代码+栈+数据共享) */
    size_t   mem_size;     /* guest 内存大小 */
    struct pe_image *current_image; /* 当前 PE 镜像, 用于 IAT 区间判断 */
    uint32_t exit_code;    /* ExitProcess 写入的退出码 */
    jmp_buf  exit_jmp;     /* ExitProcess/#UD longjmp 目标 */
    cpu_status_t status;   /* cpu_run 退出时的状态 */
} cpu_context_t;

/* cpu_run: 从 start_eip 开始解释执行, 直到 hlt/ExitProcess/#UD
 *   ctx:       已初始化的 CPU 上下文 (mem_base/ESP/current_image 等)
 *   start_eip: 起始 EIP (mem_base 偏移)
 *   返回: 执行状态 (ctx->status 与返回值一致) */
cpu_status_t cpu_run(cpu_context_t *ctx, uint32_t start_eip);

/* cpu_exit_process: 由 ExitProcess 内建函数调用, 触发 longjmp 回 cpu_run
 *   设置 ctx->exit_code 与 ctx->status 后 longjmp */
void cpu_exit_process(cpu_context_t *ctx, uint32_t code);

/* cpu_raise_ud: 遇到未实现指令时调用, 设置 exit_code=0xC000001D 后 longjmp */
void cpu_raise_ud(cpu_context_t *ctx);

/* ---- guest 内存访问 (unaligned-safe, 供 thunk 使用) ---- */
static inline uint32_t cpu_mem_r32(cpu_context_t *ctx, uint32_t addr) {
    uint32_t v; memcpy(&v, ctx->mem_base + addr, 4); return v;
}
static inline void cpu_mem_w32(cpu_context_t *ctx, uint32_t addr, uint32_t val) {
    memcpy(ctx->mem_base + addr, &val, 4);
}
static inline uint8_t cpu_mem_r8(cpu_context_t *ctx, uint32_t addr) {
    return ctx->mem_base[addr];
}

/* Win32 函数 thunk: 解释器通过 IAT 调用时的统一入口
 *   thunk 从 guest 栈 [ESP+4] 起读取参数, 转换 guest 指针为 host 指针,
 *   调用真实 C 实现, 将返回值写入 gpr[EAX] */
typedef void (*wine_thunk_t)(cpu_context_t *ctx);

/* 函数 id 基址: 所有 IAT 函数 id >= 此值, 避免与 guest 代码地址冲突 */
#define WINE_FUNC_ID_BASE 0x70000000u

#ifdef __cplusplus
}
#endif

#endif /* WINE_CPU_H */
