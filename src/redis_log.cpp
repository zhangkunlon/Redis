#include "redis_log.h"
#include <mutex>

static std::mutex mtx;
static LogLevel g_log_level = LogLevel::DEBUG;
void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mtx);
    g_log_level = level;
}

// 日志打印函数
void msg(LogLevel level, const char *fmt, ...) {
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list args;
    va_start(args, fmt); // 初始化 args 指向 fmt 后的第一个参数

    printf("[%s] ", level_str[(int)level]);
    vprintf(fmt, args);  // 使用 vprintf 解析格式化字符串和可变参数
    printf("\n");        // 换行

    va_end(args);        // 清理可变参数列表
}

void msg_info(const char *file, const char *func, int line, LogLevel level, const char *fmt, ...) {
    if (g_log_level > level) return;
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    va_list args;
    va_start(args, fmt); // 初始化 args 指向 fmt 后的第一个参数

    printf("[%s] %s:%d %s() ", level_str[(int)level], file, line, func);
    vprintf(fmt, args);  // 使用 vprintf 解析格式化字符串和可变参数
    printf("\n");        // 换行

    va_end(args);        // 清理可变参数列表
}