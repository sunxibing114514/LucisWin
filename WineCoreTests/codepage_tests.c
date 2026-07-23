/* codepage_tests.c — Phase 3.2 单元测试: SJIS 静态映射表
 *
 * 直接调用 cp_to_unicode(CP_SJIS), 验证静态表解码语义:
 *   - ASCII 单字节直通
 *   - 平假名 (0x82 9F-F1) 线性映射
 *   - 全角标点 (0x81 XX) 查表
 *   - 半角片假名 (0xA1-DF) 线性映射
 *   - 非法字节替换为 U+FFFD
 *   - hello.exe 完整字面量 (混合平假名+全角标点+ASCII) 解码正确
 *
 * 注意: cp_to_unicode 的 src_len 含末尾 \0, 输出含 UTF-16 \0 终止符。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "test_framework.h"
#include "wine/codepage.h"
#include <stdint.h>
#include <string.h>

/* 比较两个 UTF-16 字符串 (含终止符) */
static int u16_eq(const uint16_t *a, const uint16_t *b) {
    size_t i = 0;
    while (a[i] == b[i]) {
        if (a[i] == 0) return 1;
        i++;
    }
    return 0;
}

/* ASCII "hello\0" 应逐字节直通, 含终止符 */
TEST(cp_ascii_passthrough) {
    const char src[] = "hello";  /* 6 字节含 \0 */
    uint16_t dst[16] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 16);
    ASSERT(n == 6, "应输出 6 字符 (含\\0), 实际 %d", n);
    static const uint16_t expected[] = {'h','e','l','l','o', 0};
    ASSERT(u16_eq(dst, expected), "ASCII 直通失败");
}

/* "こんにちは" — 5 个平假名, 验证 0x82 9F-F1 线性映射 */
TEST(cp_sjis_hiragana_konnichiwa) {
    /* こ(82B1) ん(82F1) に(82C9) ち(82BF) は(82CD) + \0 */
    const char src[] = "\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd";
    uint16_t dst[16] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 16);
    /* 5 双字节 + 终止符 = 6 */
    ASSERT(n == 6, "应输出 6 字符 (5 平假名+\\0), 实际 %d", n);
    static const uint16_t expected[] = {
        0x3053, 0x3093, 0x306B, 0x3061, 0x306F, 0
    };
    ASSERT(u16_eq(dst, expected),
           "平假名映射错误: %04x %04x %04x %04x %04x",
           dst[0], dst[1], dst[2], dst[3], dst[4]);
}

/* 全角标点: 、(8141) → U+3001, ！(8149) → U+FF01 */
TEST(cp_sjis_fullwidth_punct) {
    const char src[] = "\x81\x41\x81\x49";  /* 、！ + \0 */
    uint16_t dst[16] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 16);
    /* 2 双字节 + 终止符 = 3 */
    ASSERT(n == 3, "应输出 3 字符 (2 标点+\\0), 实际 %d", n);
    ASSERT(dst[0] == 0x3001, "、应映射到 U+3001, 实际 %04x", dst[0]);
    ASSERT(dst[1] == 0xFF01, "！应映射到 U+FF01, 实际 %04x", dst[1]);
}

/* 半角片假名: 0xB1 (ｱ) → U+FF71, 0xDF (ﾟ) → U+FF9F */
TEST(cp_sjis_halfwidth_kana) {
    const char src[] = "\xb1\xdf";  /* ｱ ﾟ + \0 */
    uint16_t dst[16] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 16);
    /* 2 单字节 + 终止符 = 3 */
    ASSERT(n == 3, "应输出 3 字符 (2 半角+\\0), 实际 %d", n);
    ASSERT(dst[0] == 0xFF71, "0xB1 应映射到 U+FF71, 实际 %04x", dst[0]);
    ASSERT(dst[1] == 0xFF9F, "0xDF 应映射到 U+FF9F, 实际 %04x", dst[1]);
}

/* 非法单字节 (0x80) 与未知双字节 (汉字区未内嵌) 应替换为 U+FFFD */
TEST(cp_sjis_unknown_replaced) {
    /* 0x80 非法单字节; 0x90 0x40 是未内嵌的汉字区双字节 (0x40 作 b2 被消费);
     * 末尾 \0 自然成为终止符。 */
    const char src[] = "\x80\x90\x40";
    uint16_t dst[16] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 16);
    /* 2 输出字符 (FFFD + FFFD) + 终止符 = 3 */
    ASSERT(n == 3, "应输出 3 字符 (2 FFFD+\\0), 实际 %d", n);
    ASSERT(dst[0] == 0xFFFD, "0x80 应替换为 U+FFFD, 实际 %04x", dst[0]);
    ASSERT(dst[1] == 0xFFFD, "汉字区 0x90 0x40 应替换为 U+FFFD, 实际 %04x", dst[1]);
    ASSERT(dst[2] == 0x0000, "末尾应为终止符, 实际 %04x", dst[2]);
}

/* hello.exe 完整字面量: こんにちは、iOSのWineです！ */
TEST(cp_sjis_hello_exe_literal) {
    const char src[] =
        "\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd\x81\x41"
        "\x69\x4f\x53\x82\xcc\x57\x69\x6e\x65"
        "\x82\xc5\x82\xb7\x81\x49";
    /* 5 平假名(10B) + 、(2B) + iOS(3B) + の(2B) + Wine(4B) + です(4B) + ！(2B) = 27B + \0 */
    uint16_t dst[64] = {0};
    int n = cp_to_unicode(CP_SJIS, src, sizeof(src), dst, 64);
    /* 17 字符 + 终止符 = 18 */
    ASSERT(n == 18, "应输出 18 字符 (17+\\0), 实际 %d", n);
    static const uint16_t expected[] = {
        0x3053, 0x3093, 0x306B, 0x3061, 0x306F, 0x3001,
        0x0069, 0x004F, 0x0053, 0x306E,
        0x0057, 0x0069, 0x006E, 0x0065,
        0x3067, 0x3059, 0xFF01, 0
    };
    ASSERT(u16_eq(dst, expected), "hello.exe 完整字面量解码错误");
}
