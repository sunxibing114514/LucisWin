/* codepage.c — 代码页转换 (Shift-JIS → UTF-16LE)
 *
 * Phase 1: iconv 后端
 * Phase 3.2: 内嵌静态映射表, 替换 iconv。完全自包含, 无外部依赖。
 *
 * SJIS 解码规则:
 *   单字节:
 *     0x00-0x7F  ASCII 直通
 *     0xA1-0xDF  半角片假名 → U+FF61-FF9F (线性)
 *     其余       → U+FFFD
 *   双字节 (b1 in 0x81-0x9F / 0xE0-0xEF, b2 in 0x40-0x7E / 0x80-0xFC):
 *     0x82{9F-F1} 平假名 → U+3041-3093 (线性)
 *     0x83{40-96} 片假名 → U+30A1-30F6 (线性, 处理 0x7F 跳变)
 *     0x81{41-49} 全角标点 → 查表
 *     其余       → U+FFFD (Phase 3.2 未内嵌汉字区)
 */
#include "wine/codepage.h"
#include <string.h>
#include <stdio.h>

/* ---- 全角标点表 (0x81 XX) ---- */
static const struct { uint8_t b2; uint16_t u; } k_punct[] = {
    {0x41, 0x3001},  /* 、 */
    {0x42, 0x3002},  /* 。 */
    {0x43, 0xFF0C},  /* ， */
    {0x44, 0xFF0E},  /* ． */
    {0x45, 0x30FB},  /* ・ */
    {0x46, 0xFF1A},  /* ： */
    {0x47, 0xFF1B},  /* ； */
    {0x48, 0xFF1F},  /* ？ */
    {0x49, 0xFF01},  /* ！ */
    {0x4A, 0x309B},  /* ゛ */
    {0x4B, 0x309C},  /* ゜ */
    {0x4C, 0x300C},  /* 「 */
    {0x4D, 0x300D},  /* 」 */
};
#define K_PUNCT_N (sizeof(k_punct) / sizeof(k_punct[0]))

/* 双字节 SJIS → Unicode 码点 (0 = 未映射 → U+FFFD) */
static uint16_t sjis_double_to_unicode(uint8_t b1, uint8_t b2) {
    /* 平假名: 0x82{9F-F1} → U+3041-3093 (线性, 第二字节 0x9F-0xF1 连续) */
    if (b1 == 0x82 && b2 >= 0x9F && b2 <= 0xF1) {
        return 0x3041 + (b2 - 0x9F);
    }
    /* 平假名小写续 + 标点: 0x82{40-9E} 区查表/线性 (部分内嵌) */
    if (b1 == 0x82 && b2 >= 0x40 && b2 <= 0x9E) {
        /* 0x82 40→U+FF61(半角｡) ... 0x82 4F→U+FF70(半角ｰ) — 实际为 JIS X 0201 */
        /* 0x82 50→U+FF71(ｱ) ... 0x82 9E→U+FF9F(ﾟ) 半角片假名续 */
        /* Phase 3.2: 暂用 U+FFFD (非 hello.c 用到区) */
        return 0xFFFD;
    }
    /* 片假名: 0x83{40-7E} → U+30A1-30DF, 0x83{80-96} → U+30E0-30F6 */
    if (b1 == 0x83 && b2 >= 0x40 && b2 <= 0x7E) {
        return 0x30A1 + (b2 - 0x40);
    }
    if (b1 == 0x83 && b2 >= 0x80 && b2 <= 0x96) {
        return 0x30E0 + (b2 - 0x80);
    }
    /* 全角标点 0x81 XX */
    if (b1 == 0x81) {
        for (size_t i = 0; i < K_PUNCT_N; i++) {
            if (k_punct[i].b2 == b2) return k_punct[i].u;
        }
    }
    /* 其余 (汉字区等) 未内嵌 → 替换字符 */
    return 0xFFFD;
}

/* cp_to_unicode: 多字节 → UTF-16LE
 *   src_len 含末尾 \0 (用于转换终止符, 输出含 UTF-16 \0)
 *   返回: 写入 dst 的 UTF-16 字符数 (含终止符), 0 表示失败 */
int cp_to_unicode(int cp, const char *src, size_t src_len,
                  uint16_t *dst, size_t dst_max) {
    if (!src || !dst || src_len == 0 || dst_max == 0) return 0;

    /* 非 Shift-JIS: 逐字节零扩展 (ASCII 兼容) */
    if (cp != CP_SJIS) {
        size_t i;
        for (i = 0; i < src_len && i < dst_max - 1; i++) {
            dst[i] = (uint16_t)(uint8_t)src[i];
        }
        if (i < dst_max) dst[i] = 0;
        return (int)(i + 1);
    }

    size_t i = 0, o = 0;
    while (i < src_len && o < dst_max) {
        uint8_t b = (uint8_t)src[i++];
        if (b <= 0x7F) {
            /* ASCII 直通 */
            dst[o++] = (uint16_t)b;
        } else if (b >= 0xA1 && b <= 0xDF) {
            /* 半角片假名 → U+FF61-FF9F */
            dst[o++] = (uint16_t)(0xFF61 + (b - 0xA1));
        } else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEF)) {
            /* 双字节: 读 b2 */
            if (i >= src_len) {
                /* 截断: 写 U+FFFD 后退出 */
                if (o < dst_max) dst[o++] = 0xFFFD;
                break;
            }
            uint8_t b2 = (uint8_t)src[i++];
            dst[o++] = sjis_double_to_unicode(b, b2);
        } else {
            /* 0x80, 0xF0-0xFF: 非法单字节 */
            dst[o++] = 0xFFFD;
        }
    }
    /* 确保终止符 (若 src_len 含 \0, 已被 ASCII 直通写入 0x0000) */
    if (o > 0 && dst[o - 1] != 0 && o < dst_max) {
        dst[o++] = 0;
    }
    return (int)o;
}
