// Copyright (c) 2014, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: yanshiguang02@baidu.com

#include "logging.h"

#include <assert.h>
#include <boost/bind.hpp>
#include <queue>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include "mutex.h"
#include "thread.h"
#include "timer.h"

namespace baidu {
namespace common {

int g_log_level = INFO;
uint64_t g_log_size = 0;
FILE* g_log_file = stdout;
std::string g_log_file_name;
FILE* g_warning_file = NULL;

void SetLogLevel(int level) {
    g_log_level = level;
}

class AsyncLogger {
public:
    AsyncLogger()
      : jobs_(&mu_), done_(&mu_), stopped_(false), size_(0) {
        thread_.Start(boost::bind(&AsyncLogger::AsyncWriter, this));
    }
    ~AsyncLogger() {
        stopped_ = true;
        {
            MutexLock lock(&mu_);
            jobs_.Signal();
        }
        thread_.Join();
        // close fd
    }
    void WriteLog(int log_level, const char* buffer, int32_t len) {
        MutexLock lock(&mu_);
        buffer_queue_.push(make_pair(log_level, new std::string(buffer, len)));
        jobs_.Signal();
    }
    void AsyncWriter() {
        MutexLock lock(&mu_);
        while (1) {
            int loglen = 0;
            int wflen = 0;
            while (!buffer_queue_.empty()) {
                if (g_log_size && size_ > g_log_size) {
                    if (g_log_file != stdout) {
                        fclose(g_log_file);
                        char buf [50];
                        timer::now_time_str(buf, 50);
                        std::string new_log_name((g_log_file_name + ".").append(buf));
                        g_log_file = fopen(new_log_name.c_str(), "ab");
                        remove(g_log_file_name.c_str());
                        symlink(new_log_name.c_str(), g_log_file_name.c_str());
                    }
                    size_ = 0;
                }
                int log_level = buffer_queue_.front().first;
                std::string* str = buffer_queue_.front().second;
                buffer_queue_.pop();
                mu_.Unlock();
                if (str && !str->empty()) {
                    fwrite(str->data(), 1, str->size(), g_log_file);
                    loglen += str->size();
                    if (g_warning_file && log_level >= 8) {
                        fwrite(str->data(), 1, str->size(), g_warning_file);
                        wflen += str->size();
                    }
                    if (g_log_size) size_ += str->length();
                }
                delete str;
                mu_.Lock();
            }
            if (loglen) fflush(g_log_file);
            if (wflen) fflush(g_warning_file);
            if (stopped_) {
                break;
            }
            done_.Broadcast();
            jobs_.Wait();
        }

    }
    void Flush() {
        MutexLock lock(&mu_);
        buffer_queue_.push(std::make_pair(0, reinterpret_cast<std::string*>(NULL)));
        jobs_.Signal();
        done_.Wait();
    }
private:
    Mutex mu_;
    CondVar jobs_;
    CondVar done_;
    bool stopped_;
    int64_t size_;
    Thread thread_;
    std::queue<std::pair<int, std::string*> > buffer_queue_;
};

AsyncLogger g_logger;

bool SetWarningFile(const char* path, bool append) {
    const char* mode = append ? "ab" : "wb";
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        return false;
    }
    if (g_warning_file) {
        fclose(g_warning_file);
    }
    g_warning_file = fp;
    return true;
}

bool SetLogFile(const char* path, bool append) {
    const char* mode = append ? "ab" : "wb";
    g_log_file_name.assign(path);
    char buf [50];
    timer::now_time_str(buf, 50);
    std::string current_log_name((g_log_file_name + ".").append(buf));
    FILE* fp = fopen(current_log_name.c_str(), mode);
    if (fp == NULL) {
        g_log_file = stdout;
        return false;
    }
    if (g_log_file != stdout) {
        fclose(g_log_file);
    }
    g_log_file = fp;
    symlink(current_log_name.c_str(), g_log_file_name.c_str());
    return true;
}

void SetLogSize(int size) {
    g_log_size = size << 20;
}

void Logv(int log_level, const char* format, va_list ap) {
    const uint64_t thread_id = syscall(__NR_gettid);

    // We try twice: the first time with a fixed-size stack allocated buffer,
    // and the second time with a much larger dynamically allocated buffer.
    char buffer[500];
    for (int iter = 0; iter < 2; iter++) {
        char* base;
        int bufsize;
        if (iter == 0) {
            bufsize = sizeof(buffer);
            base = buffer;
        } else {
            bufsize = 30000;
            base = new char[bufsize];
        }
        char* p = base;
        char* limit = base + bufsize;

        int32_t rlen = timer::now_time_str(p, limit - p);
        p += rlen;
        p += snprintf(p, limit - p, " %lld ", static_cast<long long unsigned int>(thread_id));

        // Print the message
        if (p < limit) {
            va_list backup_ap;
            va_copy(backup_ap, ap);
            p += vsnprintf(p, limit - p, format, backup_ap);
            va_end(backup_ap);
        }

        // Truncate to available space if necessary
        if (p >= limit) {
            if (iter == 0) {
                continue;       // Try again with larger buffer
            } else {
                p = limit - 1;
            }
        }

        // Add newline if necessary
        if (p == base || p[-1] != '\n') {
            *p++ = '\n';
        }

        assert(p <= limit);
        //fwrite(base, 1, p - base, g_log_file);
        //fflush(g_log_file);
        //if (g_warning_file && log_level >= 8) {
        //    fwrite(base, 1, p - base, g_warning_file);
        //    fflush(g_warning_file);
        //}
        g_logger.WriteLog(log_level, base, p - base);
        if (log_level == FATAL) {
            g_logger.Flush();
        }
        if (base != buffer) {
            delete[] base;
        }
        break;
    }
}

void Log(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    if (level >= g_log_level) {
        Logv(level, fmt, ap);
    }
    va_end(ap);
    if (level == FATAL) {
        abort();
    }
}

} // namespace common
} // namespace baidu

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
