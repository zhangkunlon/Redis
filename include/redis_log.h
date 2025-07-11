#pragma once
#include <stdio.h>
#include <stdarg.h>

// 日志级别
enum class LogLevel { 
            DEBUG, 
            INFO, 
            WARN, 
            ERROR 
};

// 日志函数
void set_log_level(LogLevel level);
void msg(LogLevel level, const char *fmt, ...);
void msg_info(const char *file, const char *func, int line, LogLevel level, const char *fmt, ...);
#define REDIS_LOG(LogLevel,format,...) msg_info(__FILE__, __func__, __LINE__,LogLevel,format,##__VA_ARGS__);