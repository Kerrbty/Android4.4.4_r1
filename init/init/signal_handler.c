/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <cutils/sockets.h>
#include <cutils/android_reboot.h>
#include <cutils/list.h>

#include "init.h"
#include "util.h"
#include "log.h"

static int signal_fd = -1;
static int signal_recv_fd = -1;

// 子进程终止信号处理 
static void sigchld_handler(int s)
{
    // 向signal_write_fd写入1 
    write(signal_fd, &s, 1);
}

#define CRITICAL_CRASH_THRESHOLD    4       /* if we crash >4 times ... */
#define CRITICAL_CRASH_WINDOW       (4*60)  /* ... in 4 minutes, goto recovery*/

static int wait_for_one_process(int block)
{
    pid_t pid;
    int status;
    struct service *svc;
    struct socketinfo *si;
    time_t now;
    struct listnode *node;
    struct command *cmd;

    // 等待任意子进程，如果子进程没有退出则返回0，否则则返回该子进程pid。 
    while ( (pid = waitpid(-1, &status, block ? 0 : WNOHANG)) == -1 && errno == EINTR );
    if (pid <= 0) return -1;
    INFO("waitpid returned pid %d, status = %08x\n", pid, status);

    // 根据pid查找到相应的service 
    svc = service_find_by_pid(pid);
    if (!svc) {
        ERROR("untracked pid %d exited\n", pid);
        return 0;
    }

    NOTICE("process '%s', pid %d exited\n", svc->name, pid);

    // 当flags为RESTART，且不是ONESHOT时，先kill进程组内所有的子进程或子线程 
    if (!(svc->flags & SVC_ONESHOT) || (svc->flags & SVC_RESTART)) {
        kill(-pid, SIGKILL);
        NOTICE("process '%s' killing any children in process group\n", svc->name);
    }

    /* remove any sockets we may have created */
    // 移除当前服务svc中的所有创建过的socket 
    for (si = svc->sockets; si; si = si->next) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), ANDROID_SOCKET_DIR"/%s", si->name);
        unlink(tmp);
    }

    svc->pid = 0;
    svc->flags &= (~SVC_RUNNING);

        /* oneshot processes go into the disabled state on exit,
         * except when manually restarted. */
    // 对于ONESHOT服务，使其进入disabled状态 
    if ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_RESTART)) {
        svc->flags |= SVC_DISABLED;
    }

        /* disabled and reset processes do not get restarted automatically */
    // 禁用和重置的服务，都不再自动重启 
    if (svc->flags & (SVC_DISABLED | SVC_RESET) )  {
        notify_service_state(svc->name, "stopped"); // 设置相应的service状态为stopped 
        return 0;
    }

    // 服务在4分钟内重启次数超过4次，则重启手机进入recovery模式 
    now = gettime();
    if ((svc->flags & SVC_CRITICAL) && !(svc->flags & SVC_RESTART)) {
        if (svc->time_crashed + CRITICAL_CRASH_WINDOW >= now) {
            if (++svc->nr_crashed > CRITICAL_CRASH_THRESHOLD) {
                ERROR("critical process '%s' exited %d times in %d minutes; "
                      "rebooting into recovery mode\n", svc->name,
                      CRITICAL_CRASH_THRESHOLD, CRITICAL_CRASH_WINDOW / 60);
                android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
                return 0;
            }
        } else {
            svc->time_crashed = now;
            svc->nr_crashed = 1;
        }
    }

    // 设置重启服务标志位 
    svc->flags &= (~SVC_RESTART);
    svc->flags |= SVC_RESTARTING;

    /* Execute all onrestart commands for this service. */
    // 执行当前service中所有onrestart命令 
    list_for_each(node, &svc->onrestart.commands) {
        cmd = node_to_item(node, struct command, clist);
        cmd->func(cmd->nargs, cmd->args);
    }
    // 设置相应的service状态为restarting 
    notify_service_state(svc->name, "restarting");
    return 0;
}

void handle_signal(void)
{
    char tmp[32];

    /* we got a SIGCHLD - reap and restart as needed */
    // 读取数据 
    read(signal_recv_fd, tmp, sizeof(tmp));
    while (!wait_for_one_process(0))
        ;
}

void signal_init(void)
{
    int s[2];

    // 注册SIGCHILD信号，监听子进程终止信号 
    // 当捕获信号SIGCHLD，则写入 signal_fd 
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigchld_handler;
    // SA_NOCLDSTOP 使 init 进程只有在其子进程终止时才会受到SIGCHLD信号 
    act.sa_flags = SA_NOCLDSTOP; 
    sigaction(SIGCHLD, &act, 0);

    /* 为 sigchld 处理程序创建信令机制 */
    // socket pair 创建一对已经连接的套接字 
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
        signal_fd = s[0];
        signal_recv_fd = s[1];
        fcntl(s[0], F_SETFD, FD_CLOEXEC);
        fcntl(s[0], F_SETFL, O_NONBLOCK);
        fcntl(s[1], F_SETFD, FD_CLOEXEC);
        fcntl(s[1], F_SETFL, O_NONBLOCK);
    }

    handle_signal();
}

int get_signal_fd()
{
    return signal_recv_fd;
}
