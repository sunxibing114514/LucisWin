# luciswin Phase 2.4 设计规范 — 块缓存 (预解码 uops)

**状态**: 已批准（用户选择"完整项目"推进 Phase 2）
**日期**: 2026-07-23
**依赖**: Phase 2.3（commit 7b464bc/feat，39/39 测试通过）

---

## 1. 目标与范围

### 1.1 目标

为 i386 解释器引入**基本块翻译缓存**（translation cache）：
将热点基本块预解码为 uop（微操作）序列，重复执行时跳过逐字节解码，
降低 `cpu_run` 主循环在紧循环（如 fib.exe 的迭代）中的解码开销。

### 1.2 验收标志

- `fib.exe` 端到端仍输出 `fib(20)=6765`（语义不变，回归通过）
- 新增 `block_cache_tests.c`：单测验证
  - 命中计数器递增（同 EIP 二次执行命中缓存）
  - uop 翻译结果与逐字节解码语义一致（同输入同输出）
  - 含 `UOP_FALLBACK` 的块正确降级到慢路径
- **全量回归**：39/39 既有测试仍全绿
- 可观测的解码跳过：命中块不调用 `decode_modrm`

### 1.3 非目标（不在本 Phase 范围）

- **不实现完整的 uop IR**：只覆盖 fib.exe 热循环用到的指令子集，其余指令走 fallback
- **不做寄存器分配 / 优化**：uop 是 1:1 翻译，仅省去解码，不合并/重排
- **不做 JIT**：uop 解释执行，不生成原生机器码
- **不做跨块优化**：块内优化即可，不做全局 CFG 分析
- **不支持自修改代码**：代码段视为只读；若需支持，加 generation counter 失效（本 Phase 不做）
- **不优化 thunk 路径**：IAT 调用、ExitProcess、WndProc guest 调用一律走慢路径

### 1.4 关键设计原则：安全降级

**任何无法翻译的指令或块，必须回退到既有 switch 解释器，结果与未启用缓存时完全一致。**
这是本 Phase 的核心约束：缓存只加速，不改语义。具体保证：

1. 翻译阶段遇到不支持的 opcode → 当前块在**该指令前**截断，标记块结束于 `UOP_FALLBACK`，
   执行时把 `ctx->eip` 设到该指令起点后返回主循环慢路径。
2. 块在遇到任何控制流指令（jmp/jcc/call/ret/loop）时结束，控制流指令本身也用 `UOP_FALLBACK`
   交给慢路径执行（保证 flag/栈语义与原实现逐字节一致，避免重写 jcc 的微妙差异）。
3. 缓存查找与慢路径在同一 `cpu_run` 循环内协作：每次循环先查缓存，命中则执行 uop 块，
   执行返回后 `continue`；未命中则走原 switch，并在 switch 内部把可翻译的直线指令
   opportunistically 累积到当前块缓冲（见 §3.3）。

---

## 2. uop 指令子集

仅翻译以下指令（fib.exe 热循环实测用到）。超出此集合一律 fallback。

| uop | 源指令 | 语义 |
|-----|--------|------|
| `UOP_MOV_REG_IMM` | B8-BF mov r32, imm32 | `gpr[dst] = imm` |
| `UOP_MOV_REG_REG` | 8B mov r32, r/m32 (mod=3) | `gpr[dst] = gpr[src]` |
| `UOP_MOV_MEM_REG` | 89 mov r/m32, r32 (mod!=3) | `mem_w32(ea, gpr[src])` |
| `UOP_MOV_REG_MEM` | 8B mov r32, r/m32 (mod!=3) | `gpr[dst] = mem_r32(ea)` |
| `UOP_ALU_RR` | 01/29/21/09/31/39/85 (mod=3 reg-reg) | `gpr[dst] OP= gpr[src]` + flags |
| `UOP_ALU_RM` | 01/29/.../39/85 (mod!=3, mem) | `r = mem_r32(ea); r OP= gpr[src]; mem_w32` + flags |
| `UOP_ALU_IMM8` | 83 /0,/4,/5,/6,/7 (mod=3 reg) | `gpr[dst] OP= imm` + flags |
| `UOP_INC` | 40-47 inc r32 | `gpr[dst]++` + ZF/SF/OF (CF 不变) |
| `UOP_DEC` | 48-4F dec r32 | `gpr[dst]--` + ZF/SF/OF (CF 不变) |
| `UOP_FALLBACK` | 其余/控制流 | 设 eip 后返回慢路径 |

**OP 枚举**：`ALU_ADD / ALU_SUB / ALU_AND / ALU_OR / ALU_XOR / ALU_CMP / ALU_TEST`
（CMP/TEST 不写回结果，仅设 flags）。

**mod=3 (寄存器直接)** 与 **mod!=3 (内存)** 分开编码，避免运行时分支；
内存 uop 存预解码的 modrm *form*（base_reg / index_reg / scale / disp32），
运行时 `ea = gpr[base] + gpr[index]*scale + disp`。

### 2.1 为什么控制流也 fallback

jcc 的条件判定、jmp 的 eip 修改、call/ret 的栈操作都与现有实现强耦合，
重写易引入 flag/栈失衡 bug。让控制流走慢路径，块只覆盖"纯直线算术"，
风险最低且仍能省去 fib 内层 `a=c; b=c; i++; cmp; jle` 循环体里
3-5 条算术/mov 的解码开销。

---

## 3. 架构

### 3.1 数据结构

```c
typedef enum {
    UOP_MOV_REG_IMM,
    UOP_MOV_REG_REG,
    UOP_MOV_MEM_REG,
    UOP_MOV_REG_MEM,
    UOP_ALU_RR,
    UOP_ALU_RM,
    UOP_ALU_IMM8,
    UOP_INC,
    UOP_DEC,
    UOP_FALLBACK,
} uop_op_t;

typedef enum { ALU_ADD, ALU_SUB, ALU_AND, ALU_OR, ALU_XOR, ALU_CMP, ALU_TEST } alu_op_t;

/* 预解码的内存操作数 form (modrm 解析结果, EA 运行时计算) */
typedef struct {
    uint8_t  base;     /* 基址寄存器下标 (0-7), 0xFF=无 */
    uint8_t  index;    /* 变址寄存器下标, 0xFF=无 (index==4 表示无变址) */
    uint8_t  scale;    /* 1/2/4/8 */
    uint32_t disp;    /* 位移 (已按 mod 符号扩展为 uint32) */
} mem_form_t;

typedef struct {
    uop_op_t op;
    alu_op_t alu;       /* 仅 ALU_* 用 */
    uint8_t  dst;       /* 目的寄存器下标 */
    uint8_t  src;       /* 源寄存器下标 */
    uint32_t imm;       /* 立即数 */
    mem_form_t mem;     /* 仅 *_MEM 用 */
    uint32_t fallback_eip; /* 仅 UOP_FALLBACK 用: 慢路径起点 */
} uop_t;

#define UOP_BLOCK_MAX_INSNS 32
typedef struct {
    uint32_t start_eip;       /* 块起始 guest EIP (cache key) */
    int      n_uops;          /* uop 数量 */
    int      valid;          /* 1=已翻译 */
    uop_t    uops[UOP_BLOCK_MAX_INSNS];
} block_t;

#define BLOCK_CACHE_SIZE 256
static block_t g_block_cache[BLOCK_CACHE_SIZE];
static uint64_t g_cache_hits, g_cache_misses;  /* 观测计数 */
```

### 3.2 块查找

直接映射缓存：`slot = start_eip % BLOCK_CACHE_SIZE`，比较 `start_eip`。
冲突时直接覆盖（LRU 近似，热点块自然占住槽）。

### 3.3 翻译流程（缓存未命中时）

主循环对当前 `ctx->eip`：
1. 查缓存，命中且 `valid` → 执行块（§3.4）。
2. 未命中 → 调 `block_translate(ctx, start_eip)`：
   - 从 `start_eip` 起逐条用**与 switch 相同的解码逻辑**（复用 `decode_modrm` 等）
     译成 uop，直到遇到：
     - 控制流指令（jmp/jcc/call/ret/loop/hlt/0F-prefix-jcc）→ 块在前一条结束
     - 不支持的 opcode → 块以 `UOP_FALLBACK(fallback_eip=当前 insn 起点)` 结束
     - 已达 `UOP_BLOCK_MAX_INSNS` → 块满结束
   - 块结束 EIP = 下一条要执行的指令起点（可能等于 fallback_eip 或控制流指令起点）
3. 翻译完存入缓存槽，执行块。

**翻译与执行分离**：翻译阶段不修改任何 CPU 状态，只读 guest 字节；
执行阶段才改寄存器/内存/flags。这样翻译可缓存复用。

### 3.4 块执行流程

`block_exec(ctx, &block)`：
- 遍历 `block.uops[0..n_uops)`：
  - `UOP_FALLBACK` → `ctx->eip = u.fallback_eip; return;`（回主循环慢路径）
  - 其余 uop → 按表执行，更新 `ctx->eip`（用预存的下条 eip，见下）
- 每条非控制流 uop 执行后，`ctx->eip` 推进到下一条 uop 的起点。
  为此每个 uop 存 `next_eip`（或块内按顺序推进，用 `block.uops[i+1].fallback_eip` 字段复用存 start_eip）。
  **简化**：uop 内增加 `uint32_t next_eip` 字段，执行后 `ctx->eip = u.next_eip`。
- 块内最后一条非 fallback uop 执行完，`ctx->eip` 指向块结束 EIP（控制流指令或 fallback 指令的起点），
  返回主循环，主循环走慢路径处理该指令。

### 3.5 与既有 switch 的关系

switch 主循环**完全保留**，作为 fallback 与控制流指令的唯一执行路径。
块缓存是 switch 之前的一层"快速通道"：

```
cpu_run loop:
  if eip in IAT region: ... (不变)
  block = cache_lookup(eip)
  if block hit & valid:
      block_exec(ctx, block)   // 执行直线 uop, 遇控制流/fallback 返回
      continue
  // 慢路径: 原 switch
  op = byte[eip++]
  switch(op) { ... }           // 完全不变
```

---

## 4. 测试策略 (TDD)

| 组 | 文件 | 用例 | 内容 |
|----|------|------|------|
| 块缓存单测 | WineCoreTests/block_cache_tests.c (新建) | ~6 | 命中计数/翻译正确性/fallback 降级/缓存冲突覆盖/控制流不在块内/INC-DEC 标志 |
| 回归 | (已有 39 测试) | 39 | 全绿 (含 fib/winhello/heap 端到端) |

最终预期 ~45 测试全绿。

### 4.1 单测设计要点

单测直接构造一小段 i386 字节码（在伪 mem_base 中），用 `cpu_run` 跑，
**对比启用缓存前后结果一致** + 断言 `g_cache_hits > 0`。例如：

```
mov eax, 0
.loop:
  inc eax
  cmp eax, 5
  jl .loop
  hlt
```
跑完后 `eax == 5`，且循环体（inc/cmp）所在的块应被命中 ≥4 次。

---

## 5. 文件改动清单

| 文件 | 动作 |
|------|------|
| `WineCore/translator/block_cache.h` | **新建**：uop/block/缓存公开接口 |
| `WineCore/translator/block_cache.c` | **新建**：翻译 + 执行 + 查找 |
| `WineCore/translator/interpreter.c` | 扩展：`cpu_run` 主循环前插入缓存查找；`decode_modrm` 等改为非 static 或共享 (或 block_cache 内部自带解码以解耦) |
| `WineCore/include/wine/cpu.h` | 扩展：`cpu_context_t` 加 `block_cache_active` 字段 (可选, 用于单测关闭) |
| `WineCoreTests/block_cache_tests.c` | **新建**：块缓存单测 |
| `WineCoreTests/build_tests.sh` | 加 `block_cache.c` 源 |

**解耦决策**：`block_cache.c` 自带一份与 interpreter.c 完全相同的解码辅助
（`decode_modrm` 等）的副本，避免改动 interpreter.c 的 static 函数签名（降低回归风险）。
两份解码逻辑必须保持一致，单测保证之。后续可重构合并。

---

## 6. 风险与缓解

| 风险 | 缓解 |
|------|------|
| uop 翻译与 switch 语义不一致 → fib/hello/heap 输出错 | (1) 翻译复用同套 flag helper 逻辑 (2) 控制流全 fallback (3) 全量 39 回归必须绿 (4) 单测对比启用前后 |
| 内存操作数 EA 算错 | mem_form 存 base/index/scale/disp，运行时 `ea = gpr[base] + gpr[index]*scale + disp`，与 `decode_modrm` 同式 |
| 自修改代码 (本 Phase 假设不存在) | 代码段只读；若 hello/fib/heap 无自修改则安全 (实测无) |
| 缓存槽冲突导致反复重翻译 | 直接映射 + 热点块占住槽；最坏情况退化为慢路径，正确性不受影响 |
| thunk_skip_ret / IAT 路径与缓存交互 | IAT 区间在缓存查找前处理 (与现有一致)；块内遇 `call rel32`/`call r/m32` 即 fallback |
| `interpreter.c` static 解码无法复用 | block_cache.c 自带解码副本 (§5)，单测保证一致 |

---

## 7. 演进路线

```
Phase 2.1  ✓ 指令集扩展 + fibonacci.exe
Phase 2.2  ✓ WndManager 完整路径 + winhello.exe
Phase 2.3  ✓ Heap/VirtualAlloc + heap.exe
Phase 2.4  ← 本文档: 块缓存 (预解码 uops, 安全 fallback)
```

### 7.1 后续可能延伸 (不在本 Phase)

- 扩展 uop 子集到全指令 (消除 fallback)
- 块链接 (block chaining)：块末尾直接跳下一块，省主循环分发
- 寄存器分配 / 常量折叠等窥孔优化
- generation counter 支持自修改代码失效
