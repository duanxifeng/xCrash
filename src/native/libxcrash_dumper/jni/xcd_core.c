// Copyright (c) 2019-present, iQIYI, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Created by caikelun on 2019-03-07.

#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <ucontext.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <linux/elf.h>
#include <android/log.h>
#include "queue.h"
#include "xcc_errno.h"
#include "xcc_signal.h"
#include "xcc_unwind.h"
#include "xcc_util.h"
#include "xcc_spot.h"
#include "xcd_log.h"
#include "xcd_process.h"
#include "xcd_sys.h"
#include "xcd_util.h"

static int                    xcd_core_handled      = 0;
static xcc_util_build_prop_t  xcd_core_build_prop;
static xcd_recorder_t        *xcd_core_recorder     = NULL;
static xcd_process_t         *xcd_core_proc         = NULL;
static xcc_spot_t             xcd_core_spot;
static char                  *xcd_core_log_pathname = NULL;
static char                  *xcd_core_app_id       = NULL;
static char                  *xcd_core_app_version  = NULL;
static char                  *xcd_core_dump_all_threads_whitelist = NULL;

static int xcd_core_read_stdin(const char *what, void *buf, size_t len)
{
    ssize_t rc = TEMP_FAILURE_RETRY(read(STDIN_FILENO, buf, len));
    if(-1 == rc)
    {
        XCD_LOG_ERROR("CORE: read (%s) failed, errno=%d", what, errno);
        return XCC_ERRNO_SYS;
    }
    else if((ssize_t)len != rc)
    {
        XCD_LOG_ERROR("CORE: read %zd bytes (%s), expected %zu", rc, what, len);
        return XCC_ERRNO_SYS;
    }
    return 0;
}

static int xcd_core_read_args()
{
    int r;
    
    if(0 != (r = xcd_core_read_stdin("spot", (void *)&xcd_core_spot, sizeof(xcc_spot_t)))) return r;

    if(0 == xcd_core_spot.log_pathname_len) return XCC_ERRNO_INVAL;
    if(NULL == (xcd_core_log_pathname = calloc(1, xcd_core_spot.log_pathname_len + 1))) return XCC_ERRNO_NOMEM;
    if(0 != (r = xcd_core_read_stdin("path", (void *)xcd_core_log_pathname, xcd_core_spot.log_pathname_len))) return r;
    
    if(0 == xcd_core_spot.app_id_len) return XCC_ERRNO_INVAL;
    if(NULL == (xcd_core_app_id = calloc(1, xcd_core_spot.app_id_len + 1))) return XCC_ERRNO_NOMEM;
    if(0 != (r = xcd_core_read_stdin("appid", (void *)xcd_core_app_id, xcd_core_spot.app_id_len))) return r;
    
    if(0 == xcd_core_spot.app_version_len) return XCC_ERRNO_INVAL;
    if(NULL == (xcd_core_app_version = calloc(1, xcd_core_spot.app_version_len + 1))) return XCC_ERRNO_NOMEM;
    if(0 != (r = xcd_core_read_stdin("appver", (void *)xcd_core_app_version, xcd_core_spot.app_version_len))) return r;

    if(xcd_core_spot.dump_all_threads_whitelist_len > 0)
    {
        if(NULL == (xcd_core_dump_all_threads_whitelist = calloc(1, xcd_core_spot.dump_all_threads_whitelist_len + 1))) return XCC_ERRNO_NOMEM;
        if(0 != (r = xcd_core_read_stdin("whitelist", (void *)xcd_core_dump_all_threads_whitelist, xcd_core_spot.dump_all_threads_whitelist_len))) return r;
    }
    
#if 0
    __android_log_print(ANDROID_LOG_DEBUG, "xcrash_dumper",
                        "CORE: read args OK, "
                        "crash_pid=%d, "
                        "crash_tid=%d, "
                        "start_time=%"PRIu64", "
                        "crash_time=%"PRIu64", "
                        "logcat_system_lines=%u, "
                        "logcat_events_lines=%u, "
                        "logcat_main_lines=%u, "
                        "dump_map=%d, "
                        "dump_fds=%d, "
                        "dump_all_threads=%d, "
                        "dump_all_threads_count_max=%d, "
                        "log_pathname_len=%zu, "
                        "app_id_len=%zu, "
                        "app_version_len=%zu, "
                        "dump_all_threads_whitelist_len=%zu, "
                        "log_pathname=%s, "
                        "app_id=%s, "
                        "app_version=%s, "
                        "dump_all_threads_whitelist=%s",
                        xcd_core_spot.crash_pid,
                        xcd_core_spot.crash_tid,
                        xcd_core_spot.start_time,
                        xcd_core_spot.crash_time,
                        xcd_core_spot.logcat_system_lines,
                        xcd_core_spot.logcat_events_lines,
                        xcd_core_spot.logcat_main_lines,
                        xcd_core_spot.dump_map,
                        xcd_core_spot.dump_fds,
                        xcd_core_spot.dump_all_threads,
                        xcd_core_spot.dump_all_threads_count_max,
                        xcd_core_spot.log_pathname_len,
                        xcd_core_spot.app_id_len,
                        xcd_core_spot.app_version_len,
                        xcd_core_spot.dump_all_threads_whitelist_len,
                        xcd_core_log_pathname,
                        xcd_core_app_id,
                        xcd_core_app_version,
                        NULL != xcd_core_dump_all_threads_whitelist ? xcd_core_dump_all_threads_whitelist : "(NULL)"
                        );
#endif

    return 0;
}

static void xcd_core_signal_handler(int sig, siginfo_t *si, void *uc)
{
    int   fd = -1;
    int   need_close = 0;
    char  buf[2048] = "\0";

    //only once
    if(xcd_core_handled) goto exit;
    xcd_core_handled = 1;

    //restore the default system signal handler
    if(0 != xcc_signal_unregister()) goto end;

    //open file
    fd = xcd_recorder_get_fd(xcd_core_recorder);
    if(fd < 0)
    {
        if(NULL != xcd_core_log_pathname)
        {
            fd = open(xcd_core_log_pathname, O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
            need_close = 1;
        }
    }
    if(fd < 0) goto end;

    //dump signal, code, backtrace
    if(0 != xcc_util_write_format(fd,
                                  "\n\nxcrash error debug:\n"
                                  "dumper has crashed (signal: %d, code: %d)\n",
                                  si->si_signo, si->si_code)) goto end;
    if(0 != xcc_unwind_get(uc, NULL, buf, sizeof(buf))) goto end;
    if(0 != xcc_util_write_str(fd, buf)) goto end;
    if(0 != xcc_util_write_str(fd, "\n")) goto end;

 end:
    if(need_close && fd >= 0) close(fd);
    xcc_signal_raise(sig);
    return;

 exit:
    _exit(1);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    //don't leave a zombie process
    alarm(30);

    //register signal handler for catching self-crashing
    xcc_signal_register(xcd_core_signal_handler);

    //read info from pipe
    if(0 != xcd_core_read_args()) exit(103);

    //create process object
    if(0 != xcd_process_create(&xcd_core_proc,
                               xcd_core_spot.crash_pid,
                               xcd_core_spot.crash_tid,
                               &(xcd_core_spot.siginfo),
                               &(xcd_core_spot.ucontext))) exit(104);

    //suspend all threads in the process
    xcd_process_suspend_threads(xcd_core_proc);

    //load build property
    xcc_util_load_build_prop(&xcd_core_build_prop);

    //load info
    if(0 != xcd_process_load_info(xcd_core_proc)) exit(105);

    //create recorder
    if(0 != xcd_recorder_create(&xcd_core_recorder,
                                xcd_core_log_pathname)) exit(106);

    //record system info
    if(0 != xcd_sys_record(xcd_core_recorder,
                           xcd_core_spot.start_time,
                           xcd_core_spot.crash_time,
                           xcd_core_app_id,
                           xcd_core_app_version,
                           &xcd_core_build_prop,
                           xcd_process_get_number_of_threads(xcd_core_proc))) exit(107);

    //record process info
    if(0 != xcd_process_record(xcd_core_proc,
                               xcd_core_recorder,
                               xcd_core_spot.logcat_system_lines,
                               xcd_core_spot.logcat_events_lines,
                               xcd_core_spot.logcat_main_lines,
                               xcd_core_spot.dump_map,
                               xcd_core_spot.dump_fds,
                               xcd_core_spot.dump_all_threads,
                               xcd_core_spot.dump_all_threads_count_max,
                               xcd_core_dump_all_threads_whitelist,
                               xcd_core_build_prop.api_level)) exit(108);

    //free recorder
    if(NULL != xcd_core_recorder)
        xcd_recorder_destroy(&xcd_core_recorder);

    //resume all threads in the process
    xcd_process_resume_threads(xcd_core_proc);

#if XCD_CORE_DEBUG
    XCD_LOG_DEBUG("CORE: done");
#endif
    return 0;
}
