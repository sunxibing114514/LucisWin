/* WineRunner.h — ObjC 桥接 Swift, 暴露 wine_run_exe
 *
 * Phase 1: 加载 app bundle 内的 hello.exe, 调用 wine_run_exe 解释执行。
 *          MessageBoxA 调用时通过钩子弹出 UIAlertController (短路, 非 Win32 窗口)。
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface WineRunner : NSObject

/* 运行 bundle 内 hello.exe, 完成后回调 (exitCode 为 ExitProcess 参数) */
- (void)runHelloExeWithCompletion:(void(^)(int exitCode, NSString *log))completion;

@end

NS_ASSUME_NONNULL_END
