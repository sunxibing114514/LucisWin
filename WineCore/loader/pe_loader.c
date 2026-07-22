/* pe_loader.c — PE32 加载器实现
 *
 * 实现流程:
 *   1. 验证 DOS (MZ) 与 PE (\0\0EP) 签名, 校验 Machine=I386, Magic=PE32
 *   2. 分配 SizeOfImage 内存, 复制 PE headers 与各节内容
 *   3. 应用基址重定位 (HIGHLOW 类型, 32 位绝对地址修正)
 *   4. 解析导入表, 通过 wine_builtin_lookup 填充 IAT 槽 (C 函数指针)
 *
 * 性能优化:
 *   - 重定位批量处理: 同一页内连续 entry 共享一次 base 计算
 *   - IAT 直接写函数地址, 解释器无需间接跳转表
 *   - 节表解析从 base 内存直接读取, 无需额外缓存结构
 */
#include "wine/pe_loader.h"
#include "wine/wine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* PE 常量 */
#define IMAGE_DOS_SIGNATURE    0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE     0x00004550  /* "PE\0\0" */
#define IMAGE_FILE_MACHINE_I386 0x014C
#define IMAGE_OPTIONAL_HDR32_MAGIC 0x010B  /* PE32 */
#define IMAGE_ORDINAL_FLAG32   0x80000000u

/* DOS 头 (仅取所需字段) */
typedef struct {
    uint16_t e_magic;      /* 0x00: "MZ" */
    uint8_t  _pad[58];     /* 0x02 - 0x3b */
    uint32_t e_lfanew;    /* 0x3c: PE 头偏移 */
} image_dos_header_t;

/* COFF 头 (20 字节) */
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} image_file_header_t;

/* PE32 Optional Header (仅取 Phase 1 所需字段, 偏移固定) */
typedef struct {
    uint16_t Magic;
    uint8_t  _major, _minor;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;    /* +16 */
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;              /* +28 (PE32) */
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t _os_ver[2], _img_ver[2], _sub_ver[2];
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;            /* +56 */
    uint32_t SizeOfHeaders;         /* +60 */
    /* ... DataDirectory at +96 ... */
} image_optional_header_t;

/* 节头 (40 字节) */
typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint8_t  _pad[12];
    uint32_t Characteristics;
} image_section_header_t;

/* 数据目录项 (8 字节) */
typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} image_data_directory_t;

/* 导入描述符 (20 字节) */
typedef struct {
    uint32_t OriginalFirstThunk;  /* ILT/INT (导入名表) */
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                /* DLL 名 RVA */
    uint32_t FirstThunk;          /* IAT (导入地址表) */
} image_import_descriptor_t;

/* 重定位块头 */
typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} image_base_relocation_t;

/* 重定位 entry type */
#define IMAGE_REL_BASED_ABSOLUTE 0  /* 跳过 (padding) */
#define IMAGE_REL_BASED_HIGHLOW  3  /* 32 位绝对地址修正 */
#define IMAGE_REL_BASED_DIR64    10 /* 64 位 (PE32+, Phase 1 不用) */

/* IMAGE_IMPORT_BY_NAME: Hint(2) + Name(变长 ASCIIZ) */
typedef struct {
    uint16_t Hint;
    uint8_t  Name[1];
} image_import_by_name_t;

/* ============================================================ */

static pe_error_t g_last_err = PE_OK;

const char *pe_last_error_string(pe_error_t err) {
    switch (err) {
        case PE_OK:                      return "ok";
        case PE_ERR_BAD_DOS_HEADER:      return "bad DOS header (no MZ)";
        case PE_ERR_BAD_PE_SIGNATURE:    return "bad PE signature";
        case PE_ERR_NOT_PE32:            return "not PE32 (optional magic)";
        case PE_ERR_BAD_MACHINE:         return "machine != I386";
        case PE_ERR_OUT_OF_MEMORY:       return "out of memory";
        case PE_ERR_BAD_SECTION:         return "bad section";
        case PE_ERR_RESOLVE_IMPORT:      return "import resolve (partial)";
    }
    return "unknown";
}

/* 在 base 内存中, 根据 RVA 找到节并返回内存指针
 * 通过重新解析 PE 头定位节表 (避免在 image 中缓存节结构) */
void *pe_rva_to_ptr(pe_image_t *img, uint32_t rva) {
    if (!img || !img->base) return NULL;
    uint8_t *base = (uint8_t *)img->base;
    uint32_t e_lfanew = *(uint32_t *)(base + 0x3c);
    image_file_header_t *fh = (image_file_header_t *)(base + e_lfanew + 4);
    image_section_header_t *sec = (image_section_header_t *)
        ((uint8_t *)fh + sizeof(image_file_header_t) + fh->SizeOfOptionalHeader);
    for (uint32_t i = 0; i < fh->NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t vs = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        /* 节对齐扩展到 SectionAlignment, 此处用 max(vs, raw) 近似 */
        if (rva >= va && rva < va + (vs ? vs : 0x1000)) {
            return base + va + (rva - va);
        }
    }
    return NULL;
}

void *pe_get_section_by_name(pe_image_t *img, const char *name) {
    if (!img || !img->base || !name) return NULL;
    uint8_t *base = (uint8_t *)img->base;
    uint32_t e_lfanew = *(uint32_t *)(base + 0x3c);
    image_file_header_t *fh = (image_file_header_t *)(base + e_lfanew + 4);
    image_section_header_t *sec = (image_section_header_t *)
        ((uint8_t *)fh + sizeof(image_file_header_t) + fh->SizeOfOptionalHeader);
    for (uint32_t i = 0; i < fh->NumberOfSections; i++) {
        if (strncmp((const char *)sec[i].Name, name, 8) == 0) {
            return base + sec[i].VirtualAddress;
        }
    }
    return NULL;
}

/* apply_relocations: 应用基址重定位
 *   delta = base - ImageBase, 遍历重定位表, 对每个 HIGHLOW entry:
 *   *(uint32_t*)(base + page_rva + offset) += delta
 *   优化: 同一 page 内的 entry 共享 page_rva 计算 */
static void apply_relocations(pe_image_t *img, uint32_t reloc_rva, uint32_t reloc_size) {
    if (reloc_rva == 0 || reloc_size == 0) return;
    uint8_t *base = (uint8_t *)img->base;
    /* 重定位表 RVA 应能在已映射内存中直接访问 */
    image_base_relocation_t *blk = (image_base_relocation_t *)(base + reloc_rva);
    image_base_relocation_t *end = (image_base_relocation_t *)(base + reloc_rva + reloc_size);
    /* 重定位 delta = -(ImageBase): 将所有硬编码虚拟地址 (ImageBase+RVA)
     * 转为 mem_base 偏移 (纯 RVA), 使解释器可直接用 mem_base + guest_addr 访问。
     * 不依赖实际 malloc 返回的 host 地址, 64 位宿主也安全。 */
    int32_t delta = -(int32_t)img->image_base;

    while (blk < end && blk->SizeOfBlock >= sizeof(*blk)) {
        uint32_t page_rva = blk->VirtualAddress;
        uint32_t count = (blk->SizeOfBlock - sizeof(*blk)) / 2;  /* 每个 entry 2 字节 */
        uint16_t *entries = (uint16_t *)((uint8_t *)blk + sizeof(*blk));
        for (uint32_t i = 0; i < count; i++) {
            uint16_t e = entries[i];
            int type = e >> 12;
            int offset = e & 0x0FFF;
            if (type == IMAGE_REL_BASED_HIGHLOW) {
                uint32_t *fixup = (uint32_t *)(base + page_rva + offset);
                *fixup += (uint32_t)delta;
            }
            /* type == ABSOLUTE (0) 是 padding, 跳过 */
        }
        blk = (image_base_relocation_t *)((uint8_t *)blk + blk->SizeOfBlock);
    }
}

/* resolve_imports: 解析导入表, 填充 IAT
 *   遍历导入描述符数组, 对每个 DLL:
 *     - 读 ILT (OriginalFirstThunk) 获取函数名/序号
 *     - wine_builtin_lookup 查找内建地址 (64 位)
 *     - wine_func_register 转为 32 位 id
 *     - 写入 IAT (FirstThunk) 对应槽 (32 位 id, 0=未解析)
 *   解释器运行时读 IAT 槽的 id, 通过 wine_func_get 查 64 位地址调用 */
static void resolve_imports(pe_image_t *img, uint32_t import_rva, uint32_t import_size) {
    if (import_rva == 0 || import_size == 0) return;
    uint8_t *base = (uint8_t *)img->base;
    image_import_descriptor_t *desc = (image_import_descriptor_t *)(base + import_rva);
    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);
        uint32_t ilt_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        uint32_t iat_rva = desc->FirstThunk;
        uint32_t *ilt = (uint32_t *)(base + ilt_rva);
        uint32_t *iat = (uint32_t *)(base + iat_rva);
        for (uint32_t i = 0; ilt[i] != 0; i++) {
            void *addr = NULL;
            if (ilt[i] & IMAGE_ORDINAL_FLAG32) {
                /* 序号导入: Phase 1 不支持, 记未解析 */
                fprintf(stderr, "[pe] %s: 序号导入 %u 未实现\n",
                        dll_name, ilt[i] & 0xFFFF);
            } else {
                image_import_by_name_t *ibn =
                    (image_import_by_name_t *)(base + ilt[i]);
                addr = wine_builtin_lookup(dll_name, (const char *)ibn->Name);
                if (!addr) {
                    fprintf(stderr, "[pe] %s: %s 未解析 (内建未提供)\n",
                            dll_name, ibn->Name);
                }
            }
            /* 写入 IAT: 64 位函数指针 -> 32 位 id (0=未解析)
             * 解释器据此调用, 规避 32 位 IAT 槽放不下 64 位地址的问题 */
            iat[i] = addr ? wine_func_register(addr) : 0;
        }
    }
}

/* pe_load_image: 主加载函数 */
pe_error_t pe_load_image(const uint8_t *data, size_t len,
                         const char *name, pe_image_t *out) {
    if (len < sizeof(image_dos_header_t)) {
        g_last_err = PE_ERR_BAD_DOS_HEADER;
        return g_last_err;
    }
    /* 1. 验证 DOS 头 */
    image_dos_header_t *dos = (image_dos_header_t *)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        g_last_err = PE_ERR_BAD_DOS_HEADER;
        return g_last_err;
    }
    uint32_t e_lfanew = dos->e_lfanew;
    if (e_lfanew + 24 > len) {
        g_last_err = PE_ERR_BAD_PE_SIGNATURE;
        return g_last_err;
    }
    /* 2. 验证 PE 签名 */
    uint32_t *nt_sig = (uint32_t *)(data + e_lfanew);
    if (*nt_sig != IMAGE_NT_SIGNATURE) {
        g_last_err = PE_ERR_BAD_PE_SIGNATURE;
        return g_last_err;
    }
    /* 3. COFF 头 */
    image_file_header_t *fh = (image_file_header_t *)(data + e_lfanew + 4);
    if (fh->Machine != IMAGE_FILE_MACHINE_I386) {
        g_last_err = PE_ERR_BAD_MACHINE;
        return g_last_err;
    }
    /* 4. Optional Header */
    image_optional_header_t *oh = (image_optional_header_t *)
        ((uint8_t *)fh + sizeof(image_file_header_t));
    if (oh->Magic != IMAGE_OPTIONAL_HDR32_MAGIC) {
        g_last_err = PE_ERR_NOT_PE32;
        return g_last_err;
    }
    /* 5. 分配 SizeOfImage 内存 (全 RW, Phase 1 解释器不直接执行 PE 代码)
     *    SizeOfImage 按 SectionAlignment 对齐, 通常远大于文件大小
     *    (含 .bss 等未初始化节的虚拟空间), 故不应与 len 比较;
     *    仅设 256MB 上限防止恶意/损坏 PE 耗尽内存 */
    uint32_t size_of_image = oh->SizeOfImage;
    if (size_of_image == 0 || size_of_image > (256u * 1024 * 1024)) {
        g_last_err = PE_ERR_BAD_SECTION;
        return g_last_err;
    }
    uint8_t *mem = (uint8_t *)calloc(1, size_of_image);
    if (!mem) {
        g_last_err = PE_ERR_OUT_OF_MEMORY;
        return g_last_err;
    }
    /* 6. 复制 headers */
    uint32_t size_of_headers = oh->SizeOfHeaders;
    if (size_of_headers > size_of_image) size_of_headers = size_of_image;
    memcpy(mem, data, size_of_headers);
    /* 7. 复制各节 */
    image_section_header_t *sec = (image_section_header_t *)
        ((uint8_t *)oh + fh->SizeOfOptionalHeader);
    for (uint32_t i = 0; i < fh->NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t rs = sec[i].SizeOfRawData;
        uint32_t ra = sec[i].PointerToRawData;
        if (rs == 0) continue;  /* .bss 等无 raw data, 已 calloc 清零 */
        if (va + rs > size_of_image || ra + rs > len) {
            /* 节超界: 截断 (mingw 偶有节 SizeOfRawData > VirtualSize 的情况) */
            rs = size_of_image - va;
            if (ra + rs > len) rs = len - ra;
        }
        memcpy(mem + va, data + ra, rs);
    }
    /* 8. 应用重定位 (data dir[5]) */
    image_data_directory_t *dirs = (image_data_directory_t *)
        ((uint8_t *)oh + 96);  /* PE32: DataDirectory at +96 */
    /* 9. 填充 image 字段 */
    out->base = mem;
    out->size = size_of_image;
    out->entry_point = oh->AddressOfEntryPoint;
    out->image_base = oh->ImageBase;
    out->next = NULL;
    if (name) {
        strncpy(out->name, name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = 0;
    }
    /* 10. IAT 区间 (data dir[12]) — 解释器据此判断间接调用目标 */
    if (dirs[12].VirtualAddress != 0) {
        out->iat_base = dirs[12].VirtualAddress;
        out->iat_size = dirs[12].Size;
        out->iat = (void **)(mem + dirs[12].VirtualAddress);
    } else {
        out->iat_base = 0;
        out->iat_size = 0;
        out->iat = NULL;
    }
    /* 11. 重定位 (用已映射内存中的目录) — 在填充字段后执行, 因为需要 image_base */
    if (dirs[5].VirtualAddress != 0) {
        apply_relocations(out, dirs[5].VirtualAddress, dirs[5].Size);
    }
    /* 12. 导入解析 (填充 IAT) */
    if (dirs[1].VirtualAddress != 0) {
        resolve_imports(out, dirs[1].VirtualAddress, dirs[1].Size);
    }
    g_last_err = PE_OK;
    return PE_OK;
}

/* pe_resolve_export: Phase 1 内建 DLL 用注册表, 不解析真实 PE 导出表
 *   返回 NULL (内建函数请用 wine_builtin_lookup) */
void *pe_resolve_export(pe_image_t *img, const char *name) {
    (void)img; (void)name;
    return NULL;
}

void pe_free_image(pe_image_t *img) {
    if (img && img->base) {
        free(img->base);
        img->base = NULL;
        img->iat = NULL;
    }
}
