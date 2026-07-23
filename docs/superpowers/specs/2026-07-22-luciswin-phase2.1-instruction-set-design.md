# luciswin Phase 2.1 设计规范 — i386 指令集扩展

**状态**: 已批准（brainstorming 完成，用户选择"完整项目"推进 Phase 2）
**日期**: 2026-07-22
**依赖**: Phase 1（commit c97e00c，20/20 测试通过）
**前置 spec**: [2026-07-22-luciswin-phase1-design.md](2026-07-22-luciswin-phase1-design.md)

---

## 1. 目标与范围

### 1.1 目标

扩展 [WineCore/translator/interpreter.c](../../../WineCore/translator/interpreter.c) 的 i386 指令覆盖，使 `fibonacci.exe`（迭代计算 fib(20)=6765 + 自写 itoa + MessageBoxA 显示）能完整端到端跑通。

### 1.2 验收标志

`fibonacci.exe` 端到端：解释器执行后 MessageBoxA 钩子收到 UTF-16 文本 `fib(20)=6765`，caption=`luciswin`，flags=0，ExitProcess(0)。

### 1.3 非目标（留给后续 Phase）

- SSE2 标量指令（FP 运算）→ Phase 2.1 仅整数
- 完整 8086/386 全集（BCD、BOUND、ENTER 复杂形式等冷门指令）
- 真正的 Win32 窗口（仍走 UIAlertController 短路）→ Phase 2.2
- VirtualAlloc/分页 → Phase 2.3
- 块缓存性能优化 → Phase 2.4

### 1.4 Phase 2 子项目分解

Phase 2 拆为 4 个独立子项目，各自走 spec → 实现 循环：

| 顺序 | 子项目 | 依赖 | 状态 |
|------|--------|------|------|
| 2.1 | 指令集扩展（本文档） | 无 | 进行中 |
| 2.2 | WndManager 完整路径 | 2.1 | 待定 |
| 2.3 | TLB / 内存管理 | 2.1 | 待定 |
| 2.4 | 块缓存 | 2.1+2.2+2.3 稳定 | 待定 |

---

## 2. 架构决策

### 2.1 方案 A：增量 TDD，保持单文件 switch

延续 Phase 1 的 [interpreter.c](../../../WineCore/translator/interpreter.c) 单文件 switch 分发，按 TDD 增量添加指令 case。

**理由**：
- Phase 2.1 目标是功能扩展（能跑 fib），不是架构重构
- 不破坏已通过的 20 个测试，低风险
- interpreter.c 会膨胀到约 800 行，可接受；块缓存（2.4）阶段指令集定型后再拆

**否决方案 B（handler 函数表分发）**：前期重构有回归风险，间接调用开销，偏离已工作的代码。
**否决方案 C（按 opcode 组拆多文件）**：ModRM/flags 等共享 helper 须暴露到头文件，耦合增加。

### 2.2 guest 内存模型不变

沿用 Phase 1：guest 地址 = mem_base 偏移，重定位 delta = −ImageBase。新增指令不需改内存模型。

### 2.3 IAT 调用两种模式不变

沿用 Phase 1：`mov eax,[iat]; call eax`（FF /2 检测 id ≥ WINE_FUNC_ID_BASE）与 `call iat_addr`（主循环检测 EIP 落入 IAT 区间）。

---

## 3. 新增指令清单

按 TDD 类别分组。每类先写单指令测试（RED），再实现 case（GREEN）。

### 3.1 算术扩展

| Opcode | 指令 | 说明 |
|--------|------|------|
| F7 /4 | MUL r/m32 | 无符号乘法 EDX:EAX = EAX * r/m32 |
| F7 /5 | IMUL r/m32 | 有符号乘法 EDX:EAX = EAX * r/m32 |
| F7 /6 | DIV r/m32 | 无符号除法 EDX:EAX / r/m32 → EAX(商) EDX(余) |
| F7 /7 | IDIV r/m32 | 有符号除法 |
| 0F AF | IMUL r32, r/m32 | 双操作数有符号乘法 r32 = r32 * r/m32 |
| 69 | IMUL r32, r/m32, imm32 | 三操作数 |
| 6B | IMUL r32, r/m32, imm8 | 三操作数（符号扩展 imm8） |
| 40-47 | INC r32 | 自增 |
| 48-4F | DEC r32 | 自减 |
| F7 /3 | NEG r/m32 | 取反 |
| F7 /2 | NOT r/m32 | 按位取反 |
| 99 | CDQ | 符号扩展 EAX → EDX:EAX（除法前用） |
| 98 | CWDE | 符号扩展 AX → EAX |

### 3.2 移位与旋转

| Opcode | 指令 | 说明 |
|--------|------|------|
| C1 /4 | SHL r/m32, imm8 | 逻辑左移 |
| C1 /5 | SHR r/m32, imm8 | 逻辑右移 |
| C1 /7 | SAR r/m32, imm8 | 算术右移（保留符号） |
| C1 /0 | ROL r/m32, imm8 | 循环左移 |
| C1 /1 | ROR r/m32, imm8 | 循环右移 |
| D3 /4 | SHL r/m32, CL | 移位次数在 CL |
| D3 /5 | SHR r/m32, CL | |
| D3 /7 | SAR r/m32, CL | |
| D1 /4 | SHL r/m32, 1 | 移位 1 |
| D1 /5 | SHR r/m32, 1 | |
| D1 /7 | SAR r/m32, 1 | |

### 3.3 带扩展传送

| Opcode | 指令 | 说明 |
|--------|------|------|
| 0F BE | MOVSX r32, r/m8 | 符号扩展 8→32 |
| 0F BF | MOVSX r32, r/m16 | 符号扩展 16→32 |
| 0F B6 | MOVZX r32, r/m8 | 零扩展 8→32 |
| 0F B7 | MOVZX r32, r/m16 | 零扩展 16→32 |

### 3.4 条件跳转扩展

短跳转（70-7F）与近跳转（0F 80-8F）成对实现：

| 短 | 近 | 指令 | 条件 |
|----|-----|------|------|
| 72 | 82 | JB/JC | CF=1 |
| 73 | 83 | JNB/JNC | CF=0 |
| 74 | 84 | JZ/JE | ZF=1（已有短） |
| 75 | 85 | JNZ/JNE | ZF=0（已有短） |
| 76 | 86 | JBE/JNA | CF=1 或 ZF=1 |
| 77 | 87 | JA/JNBE | CF=0 且 ZF=0 |
| 78 | 88 | JS | SF=1 |
| 79 | 89 | JNS | SF=0 |
| 7C | 8C | JL/JNGE | SF≠OF |
| 7D | 8D | JGE/JNL | SF=OF |
| 7E | 8E | JLE/JNG | ZF=1 或 SF≠OF |
| 7F | 8F | JG/JNLE | ZF=0 且 SF=OF |

### 3.5 条件字节设置

| Opcode | 指令 |
|--------|------|
| 0F 90-9F | SETcc r/m8 |

### 3.6 栈与标志

| Opcode | 指令 | 说明 |
|--------|------|------|
| 9C | PUSHFD | 压入 EFLAGS |
| 9D | POPFD | 弹出 EFLAGS |
| 60 | PUSHA | 压入 8 个通用寄存器 |
| 61 | POPA | 弹出 8 个通用寄存器 |

### 3.7 0F 前缀两字节指令分发

主循环遇到 0F 前缀时，读第二字节进入 `case 0x0F:` 子分发，处理 MOVSX/MOVZX/SETcc/近 Jcc。

---

## 4. fibonacci.exe 测试夹具

### 4.1 源文件 TestExe/fib.c

```c
#include <windows.h>
void _pei386_runtime_relocator(void) {}

/* 自写 itoa: 无符号 -> 字符串, 避免 msvcrt sprintf 依赖 */
static char *my_itoa(unsigned int val, char *buf, int base) {
    char tmp[16];
    int i = 0, j = 0;
    if (val == 0) { buf[0]='0'; buf[1]=0; return buf; }
    while (val > 0) {
        int d = val % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
        val /= base;
    }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

/* 迭代 fib */
static unsigned int fib(unsigned int n) {
    if (n < 2) return n;
    unsigned int a = 0, b = 1;
    for (unsigned int i = 2; i <= n; i++) {
        unsigned int c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(void) {
    unsigned int n = 20;
    unsigned int r = fib(n);              /* 6765 */
    char buf[64];
    char *p = buf;
    const char *s = "fib(";
    while (*s) *p++ = *s++;               /* 串复制 */
    my_itoa(n, p, 10);                    /* "20" */
    while (*p) p++;
    *p++ = '=';
    my_itoa(r, p, 10);                    /* "6765" */
    MessageBoxA(NULL, buf, "luciswin", MB_OK);
    ExitProcess(0);
    return 0;
}
```

### 4.2 编译脚本 TestExe/build_fib.sh

与 [build_hello.sh](../../../TestExe/build_hello.sh) 同构：

```bash
i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 \
    -nostartfiles -e _main \
    TestExe/fib.c -o TestExe/fib.exe \
    -luser32 -lkernel32
i686-w64-mingw32-strip TestExe/fib.exe
cp TestExe/fib.exe WineCoreTests/fib.exe
cp TestExe/fib.exe LucisWinApp/Resources/fib.exe
```

关键参数沿用 Phase 1：`-nostartfiles -e _main` 跳过 CRT 启动，导入表只含 ExitProcess/atexit/MessageBoxA。

---

## 5. 测试策略

### 5.1 三组测试（TDD 红绿循环）

| 组 | 文件 | 用例数 | 内容 |
|----|------|--------|------|
| 单指令 | WineCoreTests/interpreter_tests.c | +15（总 ~26） | 每类新指令至少 1 个测试 |
| fib 端到端 | WineCoreTests/fib_end_to_end_tests.c（新建） | 3 | fib.exe 全跑 |
| 回归 | （已有 20 测试） | 20 | 原 PE/解释器/hello 端到端仍全绿 |

最终预期 ~29 测试全绿（20 原有 + 15 新单指令 - 重叠 + 3 fib 端到端 ≈ 35-40）。

### 5.2 新增单指令测试清单（最小集）

- `interp_mul_eax_edx`（F7 /4）
- `interp_imul_r32_rm32`（0F AF）
- `interp_div_edx_eax`（F7 /6）
- `interp_idiv`（F7 /7）
- `interp_inc_dec_eax`（40/48）
- `interp_neg_not`（F7 /3, /2）
- `interp_cdq`（99）
- `interp_shl_imm`（C1 /4）
- `interp_sar_imm`（C1 /7）
- `interp_shl_cl`（D3 /4）
- `interp_movsx_r32_rm8`（0F BE）
- `interp_movzx_r32_rm16`（0F B7）
- `interp_jl_near_taken`（0F 8C）
- `interp_jge_near_not_taken`（0F 8D）
- `interp_setcc_al`（0F 94 SETE）

### 5.3 fib 端到端测试

```c
static const uint16_t c_expected_text[] = {
    'f','i','b','(', '2','0',')','=','6','7','6','5', 0
};

TEST(fib_e2e_messagebox) {
    /* 加载 fib.exe, 安装捕获钩子, wine_run_exe */
    ASSERT(g_hook_called == 1);
    ASSERT(u16_eq(g_text, c_expected_text));  /* "fib(20)=6765" */
}
TEST(fib_e2e_caption) { ASSERT(u16_eq(g_caption, "luciswin")); }
TEST(fib_e2e_flags)   { ASSERT(g_flags == 0); }
```

### 5.4 验证命令

```bash
bash WineCoreTests/build_tests.sh   # Linux/macOS 快速验证
xcodebuild test -scheme WineCoreTests -destination 'platform=macOS'
```

---

## 6. 文件改动清单

| 文件 | 动作 |
|------|------|
| `WineCore/translator/interpreter.c` | 扩展 switch，新增 ~15 类指令 + 0F 前缀子分发 |
| `WineCore/include/wine/cpu.h` | 可能新增 PF/AF 标志位计算辅助 |
| `TestExe/fib.c` | 新建 fib 测试夹具 |
| `TestExe/build_fib.sh` | 新建 fib 编译脚本 |
| `WineCoreTests/interpreter_tests.c` | 新增 ~15 单指令测试 |
| `WineCoreTests/fib_end_to_end_tests.c` | 新建 fib 端到端测试 |
| `WineCoreTests/build_tests.sh` | 加入 fib.exe 路径支持 |
| `project.yml` | LucisWinApp 资源加 fib.exe |
| `.github/workflows/build.yml` | CI 加 fib.exe 编译步骤 |

---

## 7. 风险与缓解

| 风险 | 缓解 |
|------|------|
| fib.exe 编译产物引入未预期指令 | 反汇编确认，逐条 TDD 覆盖；未覆盖指令走 #UD，端到端测试给出 EIP |
| 除法溢出（除数为 0 或商溢出） | 实现 #DE 异常路径，复用 longjmp 机制（exit_code=0xC0000094） |
| 标志位计算错误（PF/AF） | 单指令测试覆盖；fib 用到的标志主要是 ZF/SF/CF/OF，AF/PF 仅为完整性 |
| interpreter.c 膨胀 | 可接受；Phase 2.4 块缓存阶段统一拆分 |

---

## 8. 演进路线（Phase 2 剩余子项目）

```
Phase 2.1  ← 本文档: 指令集扩展 + fibonacci.exe 验收
Phase 2.2  ：WndManager 完整路径 (CreateWindowExW/DefWindowProcW/消息循环)
Phase 2.3  ：TLB / 内存管理 (VirtualAlloc/分页)
Phase 2.4  ：块缓存 (预解码 uops, 指令集定型后拆分 interpreter.c)
```

每个子项目独立 spec → plan → 实现循环。
