/* WineRunner.m — iOS 壳核心: 加载 hello.exe + 安装 UIAlertController 钩子
 *
 * 调用链:
 *   runHelloExe → wine_run_exe(hello.exe bytes)
 *     → 解释器 → MessageBoxA thunk → user32_MessageBoxA_impl → cp_to_unicode
 *       → ios_messagebox_hook (本文件)
 *         → dispatch_async(main) → UIAlertController → semaphore_wait
 *     → ExitProcess → longjmp 回 wine_run_exe → 返回 exit code
 *
 * 线程模型:
 *   wine_run_exe 在后台队列同步执行 (阻塞), 钩子内 dispatch 到主线程展示弹窗,
 *   用 dispatch_semaphore 等待用户点击 OK, 实现"阻塞式 MessageBox"语义。
 */
#import "WineRunner.h"
#import <UIKit/UIKit.h>

#include "wine/wine.h"
#include "messagebox_hook.h"  /* WineCore/dlls/user32/ (include path 配置) */

/* ---- UTF-16 字符串长度 ---- */
static NSUInteger u16_strlen(const uint16_t *s) {
    NSUInteger n = 0;
    while (s && s[n]) n++;
    return n;
}

/* ---- 钩子状态: 弹窗展示期间用 semaphore 阻塞调用线程 ---- */
static dispatch_semaphore_t g_alert_sem;
static int g_alert_result;

/* ios_messagebox_hook: 收到 UTF-16 text/caption, 弹 UIAlertController
 *   在后台线程被调用 (wine_run_exe 所在线程), dispatch 到主线程展示后阻塞等待 */
static void ios_messagebox_hook(const uint16_t *text, const uint16_t *caption,
                                uint32_t flags, int *result) {
    g_alert_sem = dispatch_semaphore_create(0);
    g_alert_result = 1; /* 默认 IDOK */

    NSString *title = caption
        ? [NSString stringWithCharacters:caption length:u16_strlen(caption)]
        : @"Message";
    NSString *message = text
        ? [NSString stringWithCharacters:text length:u16_strlen(text)]
        : @"";

    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController
            alertControllerWithTitle:title
                             message:message
                      preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction
            actionWithTitle:@"OK"
                      style:UIAlertActionStyleDefault
                    handler:^(UIAlertAction *_) {
                g_alert_result = 1; /* IDOK */
                dispatch_semaphore_signal(g_alert_sem);
            }]];
        /* 找到当前 keyWindow 的 rootViewController 来 present */
        UIViewController *root = nil;
        for (UIWindowScene *scene in [UIApplication.sharedApplication.connectedScenes allObjects]) {
            if (scene.activationState == UISceneActivationStateForegroundActive) {
                for (UIWindow *w in scene.windows) {
                    if (w.isKeyWindow) { root = w.rootViewController; break; }
                }
                if (root) break;
            }
        }
        if (root) {
            [root presentViewController:alert animated:YES completion:nil];
        } else {
            /* 无可用 VC, 直接放行 */
            dispatch_semaphore_signal(g_alert_sem);
        }
    });

    /* 阻塞等待用户点击 OK */
    dispatch_semaphore_wait(g_alert_sem, DISPATCH_TIME_FOREVER);
    if (result) *result = g_alert_result;
}

@implementation WineRunner

- (instancetype)init {
    self = [super init];
    if (self) {
        /* 初始化 Wine 运行时 (注册内建 DLL) */
        wine_init();
        /* 安装 UIAlertController 钩子 */
        user32_set_messagebox_hook(ios_messagebox_hook);
    }
    return self;
}

- (void)runHelloExeWithCompletion:(void(^)(int, NSString *))completion {
    /* 在后台队列执行 wine_run_exe (阻塞调用) */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        /* 从 app bundle 加载 hello.exe */
        NSString *path = [[NSBundle mainBundle] pathForResource:@"hello" ofType:@"exe"];
        NSMutableString *log = [NSMutableString string];
        int exitCode = -1;

        if (!path) {
            [log appendString:@"错误: bundle 中未找到 hello.exe\n"];
        } else {
            NSData *data = [NSData dataWithContentsOfFile:path options:0 error:nil];
            if (!data) {
                [log appendFormat:@"错误: 无法读取 %@\n", path];
            } else {
                [log appendFormat:@"加载 hello.exe: %lu 字节\n", (unsigned long)data.length];
                [log appendString:@"启动 Wine 解释器...\n"];
                exitCode = wine_run_exe(data.bytes, data.length);
                [log appendFormat:@"ExitProcess(%d) — 执行完成\n", exitCode];
            }
        }

        /* 回主线程回调 */
        dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) completion(exitCode, log);
        });
    });
}

@end
