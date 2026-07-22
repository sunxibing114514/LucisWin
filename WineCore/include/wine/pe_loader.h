/* pe_loader.h — PE 加载器公开 API
 *
 * 功能: 解析 PE32 (i386) 可执行文件, 将各节映射到内存, 解析导入表并填充 IAT,
 *       处理基址重定位, 暴露导出符号查询。
 *
 * Phase 1 范围:
 *   - 仅 PE32 (Optional Header Magic 0x10b), 不支持 PE32+ (64 位)
 *   - 导入表递归解析, 查内建 DLL (ntdll/kernel32/user32) 导出表
 *   - 基址重定位 (IMAGE_REL_BASED_DIR64 等批量处理)
 *   - IAT 槽存内建函数的 C 函数指针, 解释器据此直接调用
 *
 * 性能优化:
 *   - 重定位批量处理 (同页内连续 delta 应用)
 *   - IAT 直接写入函数地址, 避免运行时间接跳转开销
 */
#ifndef WINE_PE_LOADER_H
#define WINE_PE_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PE image 句柄: 一个已加载到内存的 PE 模块 */
typedef struct pe_image pe_image_t;

struct pe_image {
    void       *base;          /* mmap/分配基址 (= ImageBase 加载后的实际地址) */
    uint32_t    size;          /* 已加载镜像总大小 (SizeOfImage) */
    uint32_t    entry_point;  /* 入口点 RVA (AddressOfEntryPoint) */
    uint32_t    image_base;    /* PE 头中声明的首选 ImageBase (用于重定位 delta 计算) */
    /* IAT 区: PE32 槽为 32 位, 但宿主是 64 位 (arm64/x86_64), 函数指针 8 字节放不下。
     * 故 IAT 槽存 wine_func_register 返回的 32 位 id, 解释器通过 wine_func_get(id)
     * 查 64 位函数地址。iat 指向 IAT 区内存, 使用时 cast 为 uint32_t*。 */
    void       *iat;           /* IAT 区起始指针 (槽为 uint32_t) */
    uint32_t    iat_base;      /* IAT 起始 RVA (解释器据此判断 call 目标是否在 IAT 区间) */
    uint32_t    iat_size;      /* IAT 字节长度 */
    char        name[16];      /* 模块名 (如 "hello.exe") */
    struct pe_image *next;     /* 模块链表 (一个进程可加载多个 PE) */
};

/* 加载错误码 */
typedef enum {
    PE_OK = 0,
    PE_ERR_BAD_DOS_HEADER,     /* MZ 签名错误 */
    PE_ERR_BAD_PE_SIGNATURE,   /* PE\0\0 签名错误 */
    PE_ERR_NOT_PE32,            /* 非 PE32 (可能是 PE32+) */
    PE_ERR_BAD_MACHINE,         /* Machine != I386 */
    PE_ERR_OUT_OF_MEMORY,
    PE_ERR_BAD_SECTION,
    PE_ERR_RESOLVE_IMPORT,      /* 导入解析失败 (DLL/函数找不到, 已记录但继续) */
} pe_error_t;

/* pe_load_image: 解析并加载 PE 到内存
 *   data/len: PE 文件原始字节
 *   name:     模块名 (用于日志, 截断至 15 字符)
 *   out:      输出加载后的 image (调用方分配, 内部填充字段)
 *   返回: PE_OK 或错误码
 *   注意: 加载后 image->base 指向新分配的可读写内存, .text 段标记可执行。
 *         导入表中的内建函数地址直接写入 IAT。 */
pe_error_t pe_load_image(const uint8_t *data, size_t len,
                         const char *name, pe_image_t *out);

/* pe_resolve_export: 在已加载 image 的导出表中按名字查找函数地址
 *   img:  目标模块
 *   name: 函数名 (如 "MessageBoxA")
 *   返回: 函数地址, 找不到返回 NULL
 *   Phase 1: 内建 DLL 用静态导出表, 不依赖此函数解析真实 PE 导出。 */
void *pe_resolve_export(pe_image_t *img, const char *name);

/* pe_rva_to_ptr: 将 RVA 转换为加载后内存中的指针
 *   img: 目标模块
 *   rva: 相对虚拟地址
 *   返回: 内存指针, 越界返回 NULL */
void *pe_rva_to_ptr(pe_image_t *img, uint32_t rva);

/* pe_get_section_by_name: 按名查找节, 返回节起始内存指针 (仅查找, 不验证属性) */
void *pe_get_section_by_name(pe_image_t *img, const char *name);

/* pe_free_image: 释放 pe_load_image 分配的资源 */
void pe_free_image(pe_image_t *img);

/* pe_last_error_string: 获取最近一次错误的可读描述 */
const char *pe_last_error_string(pe_error_t err);

#ifdef __cplusplus
}
#endif

#endif /* WINE_PE_LOADER_H */
