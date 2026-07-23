# luciswin Phase 2.3 设计规范 — 内存管理 (Heap + Virtual)

**状态**: 已批准（用户选择"完整项目"推进 Phase 2）
**日期**: 2026-07-23
**依赖**: Phase 2.2（commit f5b7cfd，26/26 测试通过）

---

## 1. 目标与范围

### 1.1 目标

实现 Win32 进程级内存分配 API，使 guest 程序能动态申请/释放内存。
`heap.exe`（GetProcessHeap → HeapAlloc → 写入数据 → HeapFree → VirtualAlloc 预留
→ 写入 → VirtualFree → 拼装字符串 → MessageBoxA 显示）能完整端到端跑通。

### 1.2 验收标志

`heap.exe` 端到端：
- GetProcessHeap 返回伪堆句柄 (1)
- HeapAlloc 分配 N 字节，返回可读写的 guest 指针
- 写入数据后读回一致 (内存可读写)
- HeapFree 释放 (返回 TRUE)
- VirtualAlloc 预留 + 提交一段区间，返回对齐的 guest 指针
- VirtualFree 释放 (返回 TRUE)
- MessageBoxA 收到 "heap ok" 文本
- ExitProcess(0)

### 1.3 非目标（留给后续 Phase / 不实现）

- **不实现真正的 TLB / 页表 / 分页**：iOS 沙盒禁止 `mmap MAP_FIXED`，硬件分页无意义。
  本 Phase 仅在内存布局上预留 "动态分配区" 段，对外仍是 host malloc + guest 偏移。
- 不实现 VirtualProtect 内存保护 (属性仅记录，不强制)
- 不实现共享内存 / 内存映射文件 / file mapping
- 不实现多堆 (HeapCreate 仅返回伪句柄；所有 Heap* 走同一分配器)
- 不实现 GlobalAlloc/LocalAlloc (Win16 遗留，现代程序已弃用)
- 不实现 NtAllocateVirtualMemory (ntdll 直接调用，留给后续)
- 不实现堆碎片整理 / 合并释放

### 1.4 架构决策

延续 Phase 1/2.x 的 "host malloc + guest 偏移" 模型：

```
guest 内存布局 (单个 realloc 后的连续 buffer):
  [0, img.size)                    PE 镜像 (含 .text/.data/.rdata/.bss)
  [img.size, img.size + 256KB)     主线程栈 (向下生长)
  [img.size + 256KB, heap_base)     (空隙, 不用)
  [heap_base, heap_base + HEAP_MAX) 动态分配区 (本 Phase 新增)
```

**关键约束**：
- guest 地址 = host 指针 - mem_base，无段表/页表翻译
- 动态分配区用 host 端的简单 bump allocator + free list 实现
- 分配的块记录在宿主侧元数据表（不在 guest 内存里），guest 看到的是纯指针
- 已分配区域必须落在 `heap_base` 之后，避免与镜像/栈冲突

---

## 2. 新增组件

### 2.1 内存布局扩展 (core.c)

```c
/* 新增常量 */
#define WINE_HEAP_MAX  (4 * 1024 * 1024)  /* 4 MB 动态分配区 */

/* wine_run_exe 中扩展 realloc:
 *   原先: realloc(img.base, img.size + WINE_STACK_SIZE)
 *   现在: realloc(img.base, img.size + WINE_STACK_SIZE + WINE_HEAP_MAX)
 * 布局:
 *   [0, img.size)                        PE 镜像
 *   [img.size, img.size + STACK)          栈 (ESP 起点)
 *   [img.size + STACK, img.size + STACK + HEAP_MAX)  动态分配区
 * heap_base = img.size + WINE_STACK_SIZE
 */
```

mem_size 随之扩展，解释器的 `eip >= mem_size` 边界检查仍正确。

### 2.2 内存分配器 (WineCore/dlls/kernel32/heap.c — 新建)

简单实现，**单线程、无锁**（Phase 2 单线程假设）：

```c
/* 块元数据表 (宿主侧, 不暴露给 guest) */
typedef struct {
    uint32_t guest_addr;   /* 分配的 guest 地址 (= heap_base + offset) */
    uint32_t size;         /* 分配大小 (字节) */
    int      used;         /* 1=已分配, 0=空闲 */
    int      is_virtual;   /* 1=VirtualAlloc 来, 0=HeapAlloc 来 */
} heap_block_t;

#define WINE_HEAP_BLOCK_MAX 256
static heap_block_t g_blocks[WINE_HEAP_BLOCK_MAX];

/* bump 指针: 下一个未切分位置 (相对 heap_base 的偏移) */
static uint32_t g_heap_bump = 0;
static uint32_t g_heap_base = 0;  /* guest 地址, wine_run_exe 设置 */
```

**算法**：
- `HeapAlloc(heap, 0, size)`：bump 推进 `align(size, 8)`，记元数据，返回 `heap_base + offset`
- `HeapFree`：仅标记 `used = 0`，**不归还 bump**（简单实现，足够 Phase 2.3 测试用）
- `VirtualAlloc(addr, size, MEM_COMMIT|MEM_RESERVE, PAGE_RW)`：
  - 若 `addr == NULL`：从 bump 分配 `align(size, 4096)`，标记 `is_virtual`
  - 若 `addr != NULL` 且在已 reserve 区间内：标记已 commit，返回原 addr
- `VirtualFree(addr, 0, MEM_RELEASE)`：标记 `used = 0`

**对齐**：HeapAlloc 按 8 字节对齐，VirtualAlloc 按 4096 字节对齐。

### 2.3 kernel32 thunks (WineCore/dlls/kernel32/kernel32.c — 扩展)

| Win32 函数 | 栈布局 (stdcall, [ESP+4] 起) | 返回 |
|-----------|---------------------------|------|
| GetProcessHeap | (无参数) | 伪堆句柄 = 1 |
| HeapAlloc | heap, dwFlags, dwBytes | guest 指针, 0=失败 |
| HeapFree | heap, dwFlags, lpMem | TRUE(1)/FALSE(0) |
| HeapSize | heap, dwFlags, lpMem | 实际大小 |
| HeapCreate | flOptions, dwInitialSize, dwMaximumSize | 伪堆句柄 (=2,3,...) |
| HeapDestroy | heap | TRUE(1) (Phase 2.3 空实现) |
| VirtualAlloc | lpAddress, dwSize, flAllocationType, flProtect | guest 指针, 0=失败 |
| VirtualFree | lpAddress, dwSize, dwFreeType | TRUE(1)/FALSE(0) |
| VirtualQuery | lpAddress, lpBuffer, dwLength | 写入的字节数 |
| RtlZeroMemory | dest, len | (无返回值) |
| GetTickCount | (无) | 伪 tick (0 或单调计数) |

### 2.4 与 wine_run_exe 的集成

`wine_run_exe` 在调用 `cpu_run` 前调用 `wine_heap_reset(heap_base)`，
把分配器 bump 指针和块表清空到已知状态。

```c
/* core.c wine_run_exe 中 */
ctx.gpr[CPU_REG_ESP] = img.size + WINE_STACK_SIZE - 64;
/* ... 伪造返回地址压栈 ... */
wine_heap_reset(img.size + WINE_STACK_SIZE);  /* heap_base = 栈顶之后 */
cpu_status_t st = cpu_run(&ctx, img.entry_point);
```

### 2.5 内存读写安全

guest 通过分配得到的指针读写内存，解释器现有 `mem_r32/mem_w32` 直接访问
`mem_base + addr` 即可，无需额外翻译。分配区已包含在 `realloc` 扩展的
连续 buffer 内，guest 指针落在 `[heap_base, heap_base + HEAP_MAX)` 区间。

**不做边界检查**（Phase 2.3 不做，留给后续）：若 guest 写越界，行为未定义
（实测会污染其他块或栈，但不会段错误，因为整段是 host malloc 出来的）。

---

## 3. heap.exe 测试夹具

```c
/* heap.c — Phase 2.3 测试夹具
 * 验证: HeapAlloc/VirtualAlloc 分配内存, 写入读回, 释放, 拼装字符串显示 */
#include <windows.h>

void _pei386_runtime_relocator(void) {}

int main(void) {
    /* 1. HeapAlloc: 分配 64 字节, 写入 "heap " */
    HANDLE heap = GetProcessHeap();
    char *p1 = (char *)HeapAlloc(heap, 0, 64);
    if (!p1) ExitProcess(1);
    p1[0] = 'h'; p1[1] = 'e'; p1[2] = 'a'; p1[3] = 'p'; p1[4] = ' '; p1[5] = 0;

    /* 2. VirtualAlloc: 预留+提交 1 页, 写入 "ok" */
    char *p2 = (char *)VirtualAlloc(NULL, 4096,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p2) ExitProcess(2);
    p2[0] = 'o'; p2[1] = 'k'; p2[2] = 0;

    /* 3. 拼接到 p1 后面: "heap ok" */
    char *dst = p1 + 5;   /* 跳过 "heap " */
    dst[0] = p2[0];       /* 'o' */
    dst[1] = p2[1];       /* 'k' */
    dst[2] = 0;

    /* 4. 读回验证 (内部, 不显示) */
    if (p1[0] != 'h' || p2[1] != 'k') ExitProcess(3);

    /* 5. 释放 */
    HeapFree(heap, 0, p1);
    VirtualFree(p2, 0, MEM_RELEASE);

    /* 6. 显示结果 */
    MessageBoxA(NULL, p1, "luciswin", MB_OK);

    ExitProcess(0);
    return 0;
}
```

注意：`MessageBoxA` 在 p1 被 `HeapFree` 后再访问是 UAF，但 Phase 2.3 的
`HeapFree` 不真正归还内存（仅标记 used=0），所以读到的内容仍在。
真实 Windows 下这是 UB，但本 Phase 测试允许这样做以简化夹具。

**修正方案**：把 `HeapFree` 移到 `MessageBoxA` 之后：

```c
    MessageBoxA(NULL, p1, "luciswin", MB_OK);
    HeapFree(heap, 0, p1);
    VirtualFree(p2, 0, MEM_RELEASE);
    ExitProcess(0);
```

### 3.1 验收策略

macOS/Linux 测试用 MessageBoxA 钩子捕获文本：
- `user32_set_messagebox_hook` 捕获 "heap ok"
- 验证 `wine_run_exe` 返回 0
- 单测验证 HeapAlloc 返回非 0 指针且在 `[heap_base, heap_base+HEAP_MAX)` 区间
- 单测验证 VirtualAlloc 对齐到 4096

---

## 4. 测试策略 (TDD)

| 组 | 文件 | 用例 | 内容 |
|----|------|------|------|
| 堆单测 | WineCoreTests/heap_tests.c (新建) | ~6 | GetProcessHeap/HeapAlloc-读写/HeapFree/HeapSize/VirtualAlloc-对齐/VirtualFree |
| heap 端到端 | WineCoreTests/heap_end_to_end_tests.c (新建) | 2 | heap.exe 全跑 + 文本 "heap ok" |
| 回归 | (已有 26 测试) | 26 | 原测试仍全绿 |

最终预期 ~34 测试全绿。

---

## 5. 文件改动清单

| 文件 | 动作 |
|------|------|
| `WineCore/dlls/kernel32/heap.c` | **新建**：分配器 + 内存 thunk 实现 |
| `WineCore/include/wine/heap.h` | **新建**：分配器公开接口 (`wine_heap_reset` 等) |
| `WineCore/dlls/kernel32/kernel32.c` | 扩展：仅保留 ExitProcess (thunk 移到 heap.c 或保留均可) |
| `WineCore/core.c` | 扩展：realloc 加 WINE_HEAP_MAX、调用 wine_heap_reset |
| `TestExe/heap.c` + `build_heap.sh` | **新建**：heap 测试夹具 |
| `WineCoreTests/heap_tests.c` | **新建**：堆单测 |
| `WineCoreTests/heap_end_to_end_tests.c` | **新建**：heap 端到端 |
| `WineCoreTests/build_tests.sh` | 加 heap.exe 路径 |

---

## 6. 风险与缓解

| 风险 | 缓解 |
|------|------|
| bump allocator 不回收 → 大量分配耗尽 4MB | Phase 2.3 夹具分配量小 (<1KB)，4MB 够用；后续可加 free list |
| guest 越界写污染栈 | 不做边界检查 (与真实 Wine 一致: guest 越界是 guest 的 bug)；测试夹具不越界 |
| HeapFree 后 UAF | 测试夹具把 free 放在 MessageBoxA 之后，避免依赖未定义行为 |
| VirtualAlloc 指定非 NULL 地址 | Phase 2.3 仅支持 NULL 起分配；非 NULL 返回 0 (失败) |
| 多次 wine_run_exe 间状态泄漏 | wine_heap_reset 每次清空块表与 bump |
| realloc 失败导致 heap_base 改变 | realloc 成功后 heap_base 由 mem + img.size + STACK 计算，稳定 |

---

## 7. 演进路线

```
Phase 2.1  ✓ 指令集扩展 + fibonacci.exe
Phase 2.2  ✓ WndManager 完整路径 + winhello.exe
Phase 2.3  ← 本文档: Heap/VirtualAlloc + heap.exe
Phase 2.4  ：块缓存 (预解码 uops)
```

### 7.1 后续可能的延伸 (不在本 Phase 范围)

- free list 回收：HeapFree 把块加入空闲链表，HeapAlloc 优先复用
- 区域分配器：VirtualAlloc 支持指定地址 (在已 reserve 区间内 commit)
- 内存保护：VirtualProtect 记录 PAGE_* 属性，解释器读前检查
- 大页 / NtAllocateVirtualMemory (ntdll 直通)
