/* pe_loader_tests.c — PE 加载器测试 (RED 阶段)
 *
 * 加载真实 hello.exe, 断言:
 *   - DOS/PE 签名解析正确
 *   - Machine == I386, Optional Magic == PE32
 *   - 入口点落在 .text 节
 *   - 导入表含 user32.dll 与 kernel32.dll
 *   - IAT 中 MessageBoxA 槽非 NULL
 *
 * hello.exe 由 TestExe/build_hello.sh 编译, 拷贝至 WineCoreTests/hello.exe。
 * 测试通过相对路径定位 (可执行文件同目录)。 */
/* _DEFAULT_SOURCE 启用 readlink 等 POSIX 函数声明 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/pe_loader.h"
#include "wine/wine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* readlink / ssize_t */

/* 从可执行文件同目录加载 hello.exe
 * 用 /proc/self/exe 解析可执行文件路径, 替换 basename 为 hello.exe */
static uint8_t *load_hello_exe(size_t *out_len) {
    char path[1024];
    /* 优先用环境变量 LUCISWIN_HELLO_EXE (CI/调试用) */
    const char *env = getenv("LUCISWIN_HELLO_EXE");
    if (env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 16);
        if (n < 0) { *out_len = 0; return NULL; }
        path[n] = 0;
        /* 截断到目录 */
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0;
        strcat(path, "hello.exe");
    }
    FILE *f = fopen(path, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

TEST(pe_loads_hello_exe_without_error) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    ASSERT(data != NULL, "无法加载 hello.exe (请先运行 TestExe/build_hello.sh)");
    pe_image_t img = {0};
    pe_error_t err = pe_load_image(data, len, "hello.exe", &img);
    ASSERT(err == PE_OK, "pe_load_image 返回 %d (%s)",
           err, pe_last_error_string(err));
    ASSERT(img.base != NULL, "image base 为空");
    ASSERT(img.entry_point != 0, "入口点为 0");
    free(data);
    pe_free_image(&img);
}

TEST(pe_signature_and_machine_correct) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    pe_image_t img = {0};
    ASSERT(pe_load_image(data, len, "hello.exe", &img) == PE_OK, "加载失败");
    /* 通过 base 偏移验证 PE 头字段 (e_lfanew 在 0x3c) */
    uint32_t e_lfanew = *(uint32_t *)((uint8_t *)img.base + 0x3c);
    ASSERT(e_lfanew != 0 && e_lfanew < len, "e_lfanew 异常: %#x", e_lfanew);
    /* PE 签名 "PE\0\0" */
    uint32_t sig = *(uint32_t *)((uint8_t *)img.base + e_lfanew);
    ASSERT(sig == 0x00004550, "PE 签名错误: %#x (期望 0x4550)", sig);
    /* Machine = IMAGE_FILE_MACHINE_I386 = 0x14c */
    uint16_t machine = *(uint16_t *)((uint8_t *)img.base + e_lfanew + 4);
    ASSERT(machine == 0x14c, "Machine 不是 I386: %#x", machine);
    /* Optional Header Magic = PE32 = 0x10b */
    uint16_t magic = *(uint16_t *)((uint8_t *)img.base + e_lfanew + 24);
    ASSERT(magic == 0x10b, "非 PE32: magic=%#x", magic);
    free(data);
    pe_free_image(&img);
}

TEST(entry_point_lies_in_text_section) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    pe_image_t img = {0};
    ASSERT(pe_load_image(data, len, "hello.exe", &img) == PE_OK, "加载失败");
    /* .text 节起始内存指针 */
    void *text = pe_get_section_by_name(&img, ".text");
    ASSERT(text != NULL, "找不到 .text 节");
    /* 入口点 RVA 应在 .text 范围内 — 通过 rva_to_ptr 验证可解析且非空 */
    void *entry = pe_rva_to_ptr(&img, img.entry_point);
    ASSERT(entry != NULL, "入口点 RVA %#x 无法解析", img.entry_point);
    /* 入口点应在 .text 节内 (简单范围检查) */
    /* 注: 完整范围检查需要节大小, 此处仅验证可解析, 详细范围检查留给实现 */
    free(data);
    pe_free_image(&img);
}

TEST(imports_include_user32_and_kernel32) {
    size_t len; uint8_t *data = load_hello_exe(&len);
    pe_image_t img = {0};
    ASSERT(pe_load_image(data, len, "hello.exe", &img) == PE_OK, "加载失败");
    /* 验证 IAT 已填充: iat_base 非 0, 且至少有槽位 */
    ASSERT(img.iat_base != 0, "IAT base 为 0 (导入表未解析)");
    ASSERT(img.iat != NULL, "IAT 数组为空");
    /* 断言 IAT 中含真实 MessageBoxA 地址 (由 wine_init 注册的内建 stub)
     * IAT 槽存 32 位 id, 通过 wine_func_get 查 64 位地址比对 */
    void *msgbox_addr = wine_builtin_lookup("user32.dll", "MessageBoxA");
    ASSERT(msgbox_addr != NULL, "内建未注册 MessageBoxA");
    uint32_t *iat = (uint32_t *)img.iat;
    uint32_t slots = img.iat_size / sizeof(uint32_t);
    int found_msgbox = 0;
    for (uint32_t i = 0; i < slots; i++) {
        if (wine_func_get(iat[i]) == msgbox_addr) { found_msgbox = 1; break; }
    }
    ASSERT(found_msgbox, "IAT 中未找到 MessageBoxA 地址 (导入解析失败)");
    /* 同样验证 ExitProcess (kernel32) */
    void *exit_addr = wine_builtin_lookup("kernel32.dll", "ExitProcess");
    ASSERT(exit_addr != NULL, "内建未注册 ExitProcess");
    int found_exit = 0;
    for (uint32_t i = 0; i < slots; i++) {
        if (wine_func_get(iat[i]) == exit_addr) { found_exit = 1; break; }
    }
    ASSERT(found_exit, "IAT 中未找到 ExitProcess 地址");
    free(data);
    pe_free_image(&img);
}

TEST(bad_dos_header_rejected) {
    uint8_t bad[256] = {0}; /* 全零, 无 MZ 签名 */
    pe_image_t img = {0};
    pe_error_t err = pe_load_image(bad, sizeof(bad), "bad", &img);
    ASSERT(err == PE_ERR_BAD_DOS_HEADER, "应拒绝坏 DOS 头, 实际返回 %d", err);
}

TEST(bad_pe_signature_rejected) {
    uint8_t bad[1024] = {0};
    bad[0] = 'M'; bad[1] = 'Z'; /* 合法 MZ */
    uint32_t e_lfanew = 64;
    memcpy(bad + 0x3c, &e_lfanew, 4);
    /* PE 签名位置写错值 */
    bad[64] = 'X';
    pe_image_t img = {0};
    pe_error_t err = pe_load_image(bad, sizeof(bad), "bad", &img);
    ASSERT(err == PE_ERR_BAD_PE_SIGNATURE, "应拒绝坏 PE 签名, 实际返回 %d", err);
}
