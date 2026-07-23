/* win32_types.h — Win32 兼容类型 (Phase 2.2)
 *
 * 定义 HWND/HDC/HMENU/HINSTANCE/MSG/PAINTSTRUCT 等 32 位类型,
 * 供窗口 API thunk 与解释器使用。guest 看到的是 32 位句柄 (id),
 * 宿主侧通过句柄表映射到真实对象 (UIView 指针、绘制上下文等)。
 */
#ifndef WINE_WIN32_TYPES_H
#define WINE_WIN32_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 句柄类型: 全部是 32 位 id, 0=NULL/无效 */
typedef uint32_t HWND32;
typedef uint32_t HDC32;
typedef uint32_t HMENU32;
typedef uint32_t HINSTANCE32;
typedef uint32_t HBRUSH32;
typedef uint32_t HICON32;
typedef uint32_t HCURSOR32;

/* Win32 消息常量 (仅 Phase 2.2 用到的) */
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
#define WM_QUIT    0x0012

/* 窗口风格 (仅用到的) */
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define SW_SHOW 5

/* MSG 结构 (28 字节, guest 内存布局) */
#pragma pack(push, 1)
typedef struct {
    uint32_t hwnd;      /* 0 */
    uint32_t message;   /* 4 */
    uint32_t wParam;    /* 8 */
    uint32_t lParam;    /* 12 */
    uint32_t time;      /* 16 */
    uint32_t pt_x;      /* 20 */
    uint32_t pt_y;      /* 24 */
} win32_msg_t;

/* PAINTSTRUCT (64+ 字节, 简化: 只用前部关键字段) */
typedef struct {
    uint32_t hdc;           /* 0: 设备上下文句柄 */
    int32_t  fErase;        /* 4 */
    uint32_t rcPaint_left;   /* 8 */
    uint32_t rcPaint_top;    /* 12 */
    uint32_t rcPaint_right;  /* 16 */
    uint32_t rcPaint_bottom; /* 20 */
    int32_t  fRestore;      /* 24 */
    int32_t  fIncUpdate;    /* 28 */
    uint8_t  rgbReserved[32]; /* 32 */
} win32_paintstruct_t;

/* WNDCLASSEXW (72 字节, guest 内存布局) */
typedef struct {
    uint32_t cbSize;          /* 0 */
    uint32_t style;           /* 4 */
    uint32_t lpfnWndProc;     /* 8: guest 函数地址 */
    int32_t  cbClsExtra;      /* 12 */
    int32_t  cbWndExtra;      /* 16 */
    uint32_t hInstance;       /* 20 */
    uint32_t hIcon;           /* 24 */
    uint32_t hCursor;         /* 28 */
    uint32_t hbrBackground;   /* 32 */
    uint32_t lpszMenuName;    /* 36: guest 指针 */
    uint32_t lpszClassName;   /* 40: guest 指针 */
    uint32_t hIconSm;         /* 44 */
} win32_wndclassex_t;

/* RECT (16 字节) */
typedef struct {
    int32_t left, top, right, bottom;
} win32_rect_t;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* WINE_WIN32_TYPES_H */
