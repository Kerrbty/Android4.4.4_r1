/*
 * Copyright (C) 2008 The Android Open Source Project
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <errno.h>
#include <stdarg.h>
#include <mtd/mtd-user.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>

#include <libgen.h>

#include <cutils/list.h>
#include <cutils/android_reboot.h>
#include <cutils/sockets.h>
#include <cutils/iosched_policy.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>
#include <termios.h>

#include <sys/system_properties.h>

#include "devices.h"
#include "init.h"
#include "log.h"
#include "property_service.h"
#include "bootchart.h"
#include "signal_handler.h"
#include "keychords.h"
#include "init_parser.h"
#include "util.h"
#include "ueventd.h"
#include "watchdogd.h"

struct selabel_handle *sehandle;
struct selabel_handle *sehandle_prop;

static int property_triggers_enabled = 0;

#if BOOTCHART
static int   bootchart_count;
#endif

static char console[32];
static char bootmode[32];
static char hardware[32];
static unsigned revision = 0;
static char qemu[32];

static struct action *cur_action = NULL;
static struct command *cur_command = NULL;
static struct listnode *command_queue = NULL;

void notify_service_state(const char *name, const char *state)
{
    char pname[PROP_NAME_MAX];
    int len = strlen(name);
    if ((len + 10) > PROP_NAME_MAX)
        return;
    snprintf(pname, sizeof(pname), "init.svc.%s", name);
    property_set(pname, state);
}

static int have_console;
static char console_name[PROP_VALUE_MAX] = "/dev/console";
static time_t process_needs_restart;

static const char *ENV[32];

/* add_environment - add "key=value" to the current environment */
int add_environment(const char *key, const char *val)
{
    int n;

    for (n = 0; n < 31; n++) {
        if (!ENV[n]) {
            size_t len = strlen(key) + strlen(val) + 2;
            char *entry = malloc(len);
            snprintf(entry, len, "%s=%s", key, val);
            ENV[n] = entry;
            return 0;
        }
    }

    return 1;
}

static void zap_stdio(void)
{
    int fd;
    fd = open("/dev/null", O_RDWR);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

static void open_console()
{
    int fd;
    if ((fd = open(console_name, O_RDWR)) < 0) {
        fd = open("/dev/null", O_RDWR);
    }
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

static void publish_socket(const char *name, int fd)
{
    char key[64] = ANDROID_SOCKET_ENV_PREFIX;
    char val[64];

    strlcpy(key + sizeof(ANDROID_SOCKET_ENV_PREFIX) - 1,
            name,
            sizeof(key) - sizeof(ANDROID_SOCKET_ENV_PREFIX));
    snprintf(val, sizeof(val), "%d", fd);
    add_environment(key, val);

    /* make sure we don't close-on-exec */
    fcntl(fd, F_SETFD, 0);
}

void service_start(struct service *svc, const char *dynamic_args)
{
    struct stat s;
    pid_t pid;
    int needs_console;
    int n;
    char *scon = NULL;
    int rc;

        /* starting a service removes it from the disabled or reset
         * state and immediately takes it out of the restarting
         * state if it was in there
         */
    svc->flags &= (~(SVC_DISABLED|SVC_RESTARTING|SVC_RESET|SVC_RESTART));
    svc->time_started = 0;

        /* running processes require no additional work -- if
         * they're in the process of exiting, we've ensured
         * that they will immediately restart on exit, unless
         * they are ONESHOT
         */
    if (svc->flags & SVC_RUNNING) {
        return;
    }

    needs_console = (svc->flags & SVC_CONSOLE) ? 1 : 0;
    if (needs_console && (!have_console)) {
        ERROR("service '%s' requires console\n", svc->name);
        svc->flags |= SVC_DISABLED;
        return;
    }

    if (stat(svc->args[0], &s) != 0) {
        ERROR("cannot find '%s', disabling '%s'\n", svc->args[0], svc->name);
        svc->flags |= SVC_DISABLED;
        return;
    }

    if ((!(svc->flags & SVC_ONESHOT)) && dynamic_args) {
        ERROR("service '%s' must be one-shot to use dynamic args, disabling\n",
               svc->args[0]);
        svc->flags |= SVC_DISABLED;
        return;
    }

    if (is_selinux_enabled() > 0) {
        if (svc->seclabel) {
            scon = strdup(svc->seclabel);
            if (!scon) {
                ERROR("Out of memory while starting '%s'\n", svc->name);
                return;
            }
        } else {
            char *mycon = NULL, *fcon = NULL;

            INFO("computing context for service '%s'\n", svc->args[0]);
            rc = getcon(&mycon);
            if (rc < 0) {
                ERROR("could not get context while starting '%s'\n", svc->name);
                return;
            }

            rc = getfilecon(svc->args[0], &fcon);
            if (rc < 0) {
                ERROR("could not get context while starting '%s'\n", svc->name);
                freecon(mycon);
                return;
            }

            rc = security_compute_create(mycon, fcon, string_to_security_class("process"), &scon);
            freecon(mycon);
            freecon(fcon);
            if (rc < 0) {
                ERROR("could not get context while starting '%s'\n", svc->name);
                return;
            }
        }
    }

    NOTICE("starting '%s'\n", svc->name);

    pid = fork();

    if (pid == 0) {
        struct socketinfo *si;
        struct svcenvinfo *ei;
        char tmp[32];
        int fd, sz;

        umask(077);
        if (properties_inited()) {
            get_property_workspace(&fd, &sz);
            sprintf(tmp, "%d,%d", dup(fd), sz);
            add_environment("ANDROID_PROPERTY_WORKSPACE", tmp);
        }

        for (ei = svc->envvars; ei; ei = ei->next)
            add_environment(ei->name, ei->value);

        setsockcreatecon(scon);

        for (si = svc->sockets; si; si = si->next) {
            int socket_type = (
                    !strcmp(si->type, "stream") ? SOCK_STREAM :
                        (!strcmp(si->type, "dgram") ? SOCK_DGRAM : SOCK_SEQPACKET));
            int s = create_socket(si->name, socket_type,
                                  si->perm, si->uid, si->gid);
            if (s >= 0) {
                publish_socket(si->name, s);
            }
        }

        freecon(scon);
        scon = NULL;
        setsockcreatecon(NULL);

        if (svc->ioprio_class != IoSchedClass_NONE) {
            if (android_set_ioprio(getpid(), svc->ioprio_class, svc->ioprio_pri)) {
                ERROR("Failed to set pid %d ioprio = %d,%d: %s\n",
                      getpid(), svc->ioprio_class, svc->ioprio_pri, strerror(errno));
            }
        }

        if (needs_console) {
            setsid();
            open_console();
        } else {
            zap_stdio();
        }

#if 0
        for (n = 0; svc->args[n]; n++) {
            INFO("args[%d] = '%s'\n", n, svc->args[n]);
        }
        for (n = 0; ENV[n]; n++) {
            INFO("env[%d] = '%s'\n", n, ENV[n]);
        }
#endif

        setpgid(0, getpid());

    /* as requested, set our gid, supplemental gids, and uid */
        if (svc->gid) {
            if (setgid(svc->gid) != 0) {
                ERROR("setgid failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (svc->nr_supp_gids) {
            if (setgroups(svc->nr_supp_gids, svc->supp_gids) != 0) {
                ERROR("setgroups failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (svc->uid) {
            if (setuid(svc->uid) != 0) {
                ERROR("setuid failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (svc->seclabel) {
            if (is_selinux_enabled() > 0 && setexeccon(svc->seclabel) < 0) {
                ERROR("cannot setexeccon('%s'): %s\n", svc->seclabel, strerror(errno));
                _exit(127);
            }
        }

        if (!dynamic_args) {
            if (execve(svc->args[0], (char**) svc->args, (char**) ENV) < 0) {
                ERROR("cannot execve('%s'): %s\n", svc->args[0], strerror(errno));
            }
        } else {
            char *arg_ptrs[INIT_PARSER_MAXARGS+1];
            int arg_idx = svc->nargs;
            char *tmp = strdup(dynamic_args);
            char *next = tmp;
            char *bword;

            /* Copy the static arguments */
            memcpy(arg_ptrs, svc->args, (svc->nargs * sizeof(char *)));

            while((bword = strsep(&next, " "))) {
                arg_ptrs[arg_idx++] = bword;
                if (arg_idx == INIT_PARSER_MAXARGS)
                    break;
            }
            arg_ptrs[arg_idx] = '\0';
            execve(svc->args[0], (char**) arg_ptrs, (char**) ENV);
        }
        _exit(127);
    }

    freecon(scon);

    if (pid < 0) {
        ERROR("failed to start '%s'\n", svc->name);
        svc->pid = 0;
        return;
    }

    svc->time_started = gettime();
    svc->pid = pid;
    svc->flags |= SVC_RUNNING;

    if (properties_inited())
        notify_service_state(svc->name, "running");
}

/* The how field should be either SVC_DISABLED, SVC_RESET, or SVC_RESTART */
static void service_stop_or_reset(struct service *svc, int how)
{
    /* The service is still SVC_RUNNING until its process exits, but if it has
     * already exited it shoudn't attempt a restart yet. */
    svc->flags &= (~SVC_RESTARTING);

    if ((how != SVC_DISABLED) && (how != SVC_RESET) && (how != SVC_RESTART)) {
        /* Hrm, an illegal flag.  Default to SVC_DISABLED */
        how = SVC_DISABLED;
    }
        /* if the service has not yet started, prevent
         * it from auto-starting with its class
         */
    if (how == SVC_RESET) {
        svc->flags |= (svc->flags & SVC_RC_DISABLED) ? SVC_DISABLED : SVC_RESET;
    } else {
        svc->flags |= how;
    }

    if (svc->pid) {
        NOTICE("service '%s' is being killed\n", svc->name);
        kill(-svc->pid, SIGKILL);
        notify_service_state(svc->name, "stopping");
    } else {
        notify_service_state(svc->name, "stopped");
    }
}

void service_reset(struct service *svc)
{
    service_stop_or_reset(svc, SVC_RESET);
}

void service_stop(struct service *svc)
{
    service_stop_or_reset(svc, SVC_DISABLED);
}

void service_restart(struct service *svc)
{
    if (svc->flags & SVC_RUNNING) {
        /* Stop, wait, then start the service. */
        service_stop_or_reset(svc, SVC_RESTART);
    } else if (!(svc->flags & SVC_RESTARTING)) {
        /* Just start the service since it's not running. */
        service_start(svc, NULL);
    } /* else: Service is restarting anyways. */
}

void property_changed(const char *name, const char *value)
{
    if (property_triggers_enabled)
        queue_property_triggers(name, value);
}

static void restart_service_if_needed(struct service *svc)
{
    time_t next_start_time = svc->time_started + 5;

    if (next_start_time <= gettime()) {
        svc->flags &= (~SVC_RESTARTING);
        service_start(svc, NULL);
        return;
    }

    if ((next_start_time < process_needs_restart) ||
        (process_needs_restart == 0)) {
        process_needs_restart = next_start_time;
    }
}

static void restart_processes()
{
    process_needs_restart = 0;
    service_for_each_flags(SVC_RESTARTING,
                           restart_service_if_needed);
}

static void msg_start(const char *name)
{
    struct service *svc = NULL;
    char *tmp = NULL;
    char *args = NULL;

    if (!strchr(name, ':'))
        svc = service_find_by_name(name);
    else {
        tmp = strdup(name);
        if (tmp) {
            args = strchr(tmp, ':');
            *args = '\0';
            args++;

            svc = service_find_by_name(tmp);
        }
    }

    if (svc) {
        service_start(svc, args);
    } else {
        ERROR("no such service '%s'\n", name);
    }
    if (tmp)
        free(tmp);
}

static void msg_stop(const char *name)
{
    struct service *svc = service_find_by_name(name);

    if (svc) {
        service_stop(svc);
    } else {
        ERROR("no such service '%s'\n", name);
    }
}

static void msg_restart(const char *name)
{
    struct service *svc = service_find_by_name(name);

    if (svc) {
        service_restart(svc);
    } else {
        ERROR("no such service '%s'\n", name);
    }
}

void handle_control_message(const char *msg, const char *arg)
{
    if (!strcmp(msg,"start")) {
        msg_start(arg);
    } else if (!strcmp(msg,"stop")) {
        msg_stop(arg);
    } else if (!strcmp(msg,"restart")) {
        msg_restart(arg);
    } else {
        ERROR("unknown control msg '%s'\n", msg);
    }
}

static struct command *get_first_command(struct action *act)
{
    struct listnode *node;
    node = list_head(&act->commands);
    if (!node || list_empty(&act->commands))
        return NULL;

    return node_to_item(node, struct command, clist);
}

static struct command *get_next_command(struct action *act, struct command *cmd)
{
    struct listnode *node;
    node = cmd->clist.next;
    if (!node)
        return NULL;
    if (node == &act->commands)
        return NULL;

    return node_to_item(node, struct command, clist);
}

static int is_last_command(struct action *act, struct command *cmd)
{
    return (list_tail(&act->commands) == &cmd->clist);
}

void execute_one_command(void)
{
    int ret;

    if (!cur_action || !cur_command || is_last_command(cur_action, cur_command)) {
        // 从列表中摘取头部一个结点 
        cur_action = action_remove_queue_head();
        cur_command = NULL;
        if (!cur_action)
            return;
        INFO("processing action %p (%s)\n", cur_action, cur_action->name);
        // 获取 cur_action->commands 命令行参数  
        cur_command = get_first_command(cur_action);
    } else {
        cur_command = get_next_command(cur_action, cur_command);
    }

    if (!cur_command)
        return;
    
    // 执行 action 指定的程序，传入指定的命令行参数  
    ret = cur_command->func(cur_command->nargs, cur_command->args);
    INFO("command '%s' r=%d\n", cur_command->args[0], ret);
}

static int wait_for_coldboot_done_action(int nargs, char **args)
{
    int ret;
    INFO("wait for %s\n", coldboot_done);
    ret = wait_for_file(coldboot_done, COMMAND_RETRY_TIMEOUT);
    if (ret)
        ERROR("Timed out waiting for %s\n", coldboot_done);
    return ret;
}

/*
 * Writes 512 bytes of output from Hardware RNG (/dev/hw_random, backed
 * by Linux kernel's hw_random framework) into Linux RNG's via /dev/urandom.
 * Does nothing if Hardware RNG is not present.
 *
 * Since we don't yet trust the quality of Hardware RNG, these bytes are not
 * mixed into the primary pool of Linux RNG and the entropy estimate is left
 * unmodified.
 *
 * If the HW RNG device /dev/hw_random is present, we require that at least
 * 512 bytes read from it are written into Linux RNG. QA is expected to catch
 * devices/configurations where these I/O operations are blocking for a long
 * time. We do not reboot or halt on failures, as this is a best-effort
 * attempt.
 */
static int mix_hwrng_into_linux_rng_action(int nargs, char **args)
{
    int result = -1;
    int hwrandom_fd = -1;
    int urandom_fd = -1;
    char buf[512];
    ssize_t chunk_size;
    size_t total_bytes_written = 0;

    hwrandom_fd = TEMP_FAILURE_RETRY(
            open("/dev/hw_random", O_RDONLY | O_NOFOLLOW));
    if (hwrandom_fd == -1) {
        if (errno == ENOENT) {
          ERROR("/dev/hw_random not found\n");
          /* It's not an error to not have a Hardware RNG. */
          result = 0;
        } else {
          ERROR("Failed to open /dev/hw_random: %s\n", strerror(errno));
        }
        goto ret;
    }

    urandom_fd = TEMP_FAILURE_RETRY(
            open("/dev/urandom", O_WRONLY | O_NOFOLLOW));
    if (urandom_fd == -1) {
        ERROR("Failed to open /dev/urandom: %s\n", strerror(errno));
        goto ret;
    }

    while (total_bytes_written < sizeof(buf)) {
        chunk_size = TEMP_FAILURE_RETRY(
                read(hwrandom_fd, buf, sizeof(buf) - total_bytes_written));
        if (chunk_size == -1) {
            ERROR("Failed to read from /dev/hw_random: %s\n", strerror(errno));
            goto ret;
        } else if (chunk_size == 0) {
            ERROR("Failed to read from /dev/hw_random: EOF\n");
            goto ret;
        }

        chunk_size = TEMP_FAILURE_RETRY(write(urandom_fd, buf, chunk_size));
        if (chunk_size == -1) {
            ERROR("Failed to write to /dev/urandom: %s\n", strerror(errno));
            goto ret;
        }
        total_bytes_written += chunk_size;
    }

    INFO("Mixed %d bytes from /dev/hw_random into /dev/urandom",
                total_bytes_written);
    result = 0;

ret:
    if (hwrandom_fd != -1) {
        close(hwrandom_fd);
    }
    if (urandom_fd != -1) {
        close(urandom_fd);
    }
    memset(buf, 0, sizeof(buf));
    return result;
}

static int keychord_init_action(int nargs, char **args)
{
    keychord_init();
    return 0;
}

static int console_init_action(int nargs, char **args)
{
    int fd;

    if (console[0]) {
        snprintf(console_name, sizeof(console_name), "/dev/%s", console);
    }

    // 打开屏幕设备 
    fd = open(console_name, O_RDWR);
    if (fd >= 0)
        have_console = 1;
    close(fd);

    // 将 Android 启动 Logo 显示在LCD屏幕上 
    // 仅支持 rle565 格式的图片 
    if( load_565rle_image(INIT_IMAGE_FILE) ) {
        fd = open("/dev/tty0", O_WRONLY);
        if (fd >= 0) {
            const char *msg;
                msg = "\n"
            "\n"
            "\n"
            "\n"
            "\n"
            "\n"
            "\n"  // console is 40 cols x 30 lines
            "\n"
            "\n"
            "\n"
            "\n"
            "\n"
            "\n"
            "\n"
            "             A N D R O I D ";
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
    return 0;
}

static void import_kernel_nv(char *name, int for_emulator)
{
    char *value = strchr(name, '=');
    int name_len = strlen(name);

    if (value == 0) return;
    *value++ = 0;
    if (name_len == 0) return;

    if (for_emulator) {
        /* in the emulator, export any kernel option with the
         * ro.kernel. prefix */
        char buff[PROP_NAME_MAX];
        int len = snprintf( buff, sizeof(buff), "ro.kernel.%s", name );

        if (len < (int)sizeof(buff))
            property_set( buff, value );
        return;
    }

    if (!strcmp(name,"qemu")) {
        strlcpy(qemu, value, sizeof(qemu));
    } else if (!strncmp(name, "androidboot.", 12) && name_len > 12) {
        const char *boot_prop_name = name + 12;
        char prop[PROP_NAME_MAX];
        int cnt;

        cnt = snprintf(prop, sizeof(prop), "ro.boot.%s", boot_prop_name);
        if (cnt < PROP_NAME_MAX)
            property_set(prop, value);
    }
}

static void export_kernel_boot_props(void)
{
    char tmp[PROP_VALUE_MAX];
    int ret;
    unsigned i;
    struct {
        const char *src_prop;
        const char *dest_prop;
        const char *def_val;
    } prop_map[] = {
        { "ro.boot.serialno", "ro.serialno", "", },
        { "ro.boot.mode", "ro.bootmode", "unknown", },
        { "ro.boot.baseband", "ro.baseband", "unknown", },
        { "ro.boot.bootloader", "ro.bootloader", "unknown", },
    };

    for (i = 0; i < ARRAY_SIZE(prop_map); i++) {
        ret = property_get(prop_map[i].src_prop, tmp);
        if (ret > 0)
            property_set(prop_map[i].dest_prop, tmp);
        else
            property_set(prop_map[i].dest_prop, prop_map[i].def_val);
    }

    ret = property_get("ro.boot.console", tmp);
    if (ret)
        strlcpy(console, tmp, sizeof(console));

    /* save a copy for init's usage during boot */
    property_get("ro.bootmode", tmp);
    strlcpy(bootmode, tmp, sizeof(bootmode));

    /* if this was given on kernel command line, override what we read
     * before (e.g. from /proc/cpuinfo), if anything */
    ret = property_get("ro.boot.hardware", tmp);
    if (ret)
        strlcpy(hardware, tmp, sizeof(hardware));
    property_set("ro.hardware", hardware);

    snprintf(tmp, PROP_VALUE_MAX, "%d", revision);
    property_set("ro.revision", tmp);

    /* TODO: these are obsolete. We should delete them */
    if (!strcmp(bootmode,"factory"))
        property_set("ro.factorytest", "1");
    else if (!strcmp(bootmode,"factory2"))
        property_set("ro.factorytest", "2");
    else
        property_set("ro.factorytest", "0");
}

static void process_kernel_cmdline(void)
{
    /* don't expose the raw commandline to nonpriv processes */
    chmod("/proc/cmdline", 0440);

    /* first pass does the common stuff, and finds if we are in qemu.
     * second pass is only necessary for qemu to export all kernel params
     * as props.
     */
    import_kernel_cmdline(0, import_kernel_nv);
    if (qemu[0])
        import_kernel_cmdline(1, import_kernel_nv);

    /* now propogate the info given on command line to internal variables
     * used by init as well as the current required properties
     */
    export_kernel_boot_props();
}

// 启动属性服务 prop 
// 属性值的修改仅能在init进程中进行，即一个进程若想修改属性值，必须向 init 进程提交请求 
// inti进程生成 "/dev/socket/property_service" 套接字 
static int property_service_init_action(int nargs, char **args)
{
    /* read any property files on system or data and
     * fire up the property service.  This must happen
     * after the ro.foo properties are set above so
     * that /data/local.prop cannot interfere with them.
     */
    start_property_service();
    return 0;
}

// 处理子进程终止 
static int signal_init_action(int nargs, char **args)
{
    signal_init();
    return 0;
}

static int check_startup_action(int nargs, char **args)
{
    /* make sure we actually have all the pieces we need */
    if ((get_property_set_fd() < 0) ||
        (get_signal_fd() < 0)) {
        ERROR("init startup failure\n");
        exit(1);
    }

        /* signal that we hit this point */
    unlink("/dev/.booting");

    return 0;
}

static int queue_property_triggers_action(int nargs, char **args)
{
    queue_all_property_triggers();
    /* enable property triggers */
    property_triggers_enabled = 1;
    return 0;
}

#if BOOTCHART
static int bootchart_init_action(int nargs, char **args)
{
    bootchart_count = bootchart_init();
    if (bootchart_count < 0) {
        ERROR("bootcharting init failure\n");
    } else if (bootchart_count > 0) {
        NOTICE("bootcharting started (period=%d ms)\n", bootchart_count*BOOTCHART_POLLING_MS);
    } else {
        NOTICE("bootcharting ignored\n");
    }

    return 0;
}
#endif

static const struct selinux_opt seopts_prop[] = {
        { SELABEL_OPT_PATH, "/property_contexts" },
        { 0, NULL }
};

struct selabel_handle* selinux_android_prop_context_handle(void)
{
    int i = 0;
    struct selabel_handle* sehandle = NULL;
    while ((sehandle == NULL) && seopts_prop[i].value) {
        // 5.1 属性策略文件已经增加到2个 
        // /property_contexts 
        // /data/security/current/property_contexts 
        sehandle = selabel_open(SELABEL_CTX_ANDROID_PROP, &seopts_prop[i], 1);
        i++;
    }

    if (!sehandle) {
        ERROR("SELinux:  Could not load property_contexts:  %s\n",
              strerror(errno));
        return NULL;
    }
    INFO("SELinux: Loaded property contexts from %s\n", seopts_prop[i - 1].value);
    return sehandle;
}

void selinux_init_all_handles(void)
{
    sehandle = selinux_android_file_context_handle();
    sehandle_prop = selinux_android_prop_context_handle();
}

static bool selinux_is_disabled(void)
{
    char tmp[PROP_VALUE_MAX];

    if (access("/sys/fs/selinux", F_OK) != 0) {
        /* SELinux is not compiled into the kernel, or has been disabled
         * via the kernel command line "selinux=0".
         */
        return true;
    }

    if ((property_get("ro.boot.selinux", tmp) != 0) && (strcmp(tmp, "disabled") == 0)) {
        /* SELinux is compiled into the kernel, but we've been told to disable it. */
        return true;
    }

    return false;
}

static bool selinux_is_enforcing(void)
{
    char tmp[PROP_VALUE_MAX];

    if (property_get("ro.boot.selinux", tmp) == 0) {
        /* Property is not set.  Assume enforcing */
        return true;
    }

    if (strcmp(tmp, "permissive") == 0) {
        /* SELinux is in the kernel, but we've been told to go into permissive mode */
        return false;
    }

    if (strcmp(tmp, "enforcing") != 0) {
        ERROR("SELinux: Unknown value of ro.boot.selinux. Got: \"%s\". Assuming enforcing.\n", tmp);
    }

    return true;
}

int selinux_reload_policy(void)
{
    // 判断 /sys/fs/selinux 是否存在 
    // 判断设置 ro.boot.selinux 是否为 disabled 
    if (selinux_is_disabled()) {
        return -1;
    }

    INFO("SELinux: Attempting to reload policy files\n");

    if (selinux_android_reload_policy() == -1) {
        return -1;
    }

    if (sehandle)
        selabel_close(sehandle);

    if (sehandle_prop)
        selabel_close(sehandle_prop);

    selinux_init_all_handles();
    return 0;
}

int audit_callback(void *data, security_class_t cls, char *buf, size_t len)
{
    snprintf(buf, len, "property=%s", !data ? "NULL" : (char *)data);
    return 0;
}

static void selinux_initialize(void)
{
    // 判断 /sys/fs/selinux 是否存在 
    // 判断设置 ro.boot.selinux 是否为 disabled 
    if (selinux_is_disabled()) {
        return;
    }

    INFO("loading selinux policy\n");
    // 用于加载sepolicy文件。该函数最终将sepolicy文件传递给kernel，这样kernel就有了安全策略配置文件 
    if (selinux_android_load_policy() < 0) {
        ERROR("SELinux: Failed to load policy; rebooting into recovery mode\n");
        android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
        while (1) { pause(); }  // never reached
    }

    selinux_init_all_handles(); // 非内核启动selinux，调用该函数 
    bool is_enforcing = selinux_is_enforcing();  // prop中得到的信息 
    INFO("SELinux: security_setenforce(%d)\n", is_enforcing);
    security_setenforce(is_enforcing);
}

int main(int argc, char **argv)
{
    int fd_count = 0;
    struct pollfd ufds[4];
    char *tmpdev;
    char* debuggable;
    char tmp[32];
    int property_set_fd_init = 0;
    int signal_fd_init = 0;
    int keychord_fd_init = 0;
    bool is_charger = false;

    if (!strcmp(basename(argv[0]), "ueventd"))
        return ueventd_main(argc, argv);

    if (!strcmp(basename(argv[0]), "watchdogd"))
        return watchdogd_main(argc, argv);

    /* clear the umask */
    umask(0);

        /* Get the basic filesystem setup we need put
         * together in the initramdisk on / and then we'll
         * let the rc file figure out the rest.
         */
    // 创建并挂载启动所需的文件目录 
    // /dev /proc /sys 三个目录都是虚拟目录,源码编译不会生成,系统终止就会消失 
    mkdir("/dev", 0755);  // 硬件设备访问所需的设备驱动程序 
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    /* 
     * 生成结构如下： 
     * /
     * ├─── /dev [tmpfs] 
     * │     │
     * │     ├─ /pts [devpts] 
     * │     │
     * │     └─ /socket
     * │ 
     * ├───  /proc [proc]
     * │      
     * └───  /sys [sysfs]      
     */
     
    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");  // 一种虚拟内存的文件系统，典型的tmpfs文件系统完全驻留在RAM中，读写速度快 
    mkdir("/dev/pts", 0755);
    mkdir("/dev/socket", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);  // 一种虚拟终端文件系统 
    mount("proc", "/proc", "proc", 0, NULL);  // 虚拟文件系统，只存在于内存中，借助此文件系统，应用程序可以与内核内部数据结构进行交互 
    mount("sysfs", "/sys", "sysfs", 0, NULL);  // 在Linux2.6中引入，用于将系统中的设备组织成层次结构，并向用户模式程序提供详细的内核数据结构信息 

        /* indicate that booting is in progress to background fw loaders, etc */
    close(open("/dev/.booting", O_WRONLY | O_CREAT, 0000));

        /* We must have some place other than / to create the
         * device nodes for kmsg and null, otherwise we won't
         * be able to remount / read-only later on.
         * Now that tmpfs is mounted on /dev, we can actually
         * talk to the outside world.
         */
    // 生成 log 设备，以便输出 init 进程的运行信息 
    open_devnull_stdio();  // 创建运行日志输出设备 /dev/__null__,并将输入，输出，错误全部重定向到 __null__ 
    klog_init();  // 生成 /dev/__kmsg__ 设备，init进程可以通过 printk 内核信息输出函数来输出log信息 (用 dmesg 命令可以查看日志) 
    property_init();  // 创建并初始化属性域，属性由init进程管理，其他进程要修改必须提交请求给init进程 

    get_hardware_name(hardware, &revision);

    // 解析命令行，向属性域添加信息 
    process_kernel_cmdline();

    union selinux_callback cb;
    cb.func_log = klog_write;
    selinux_set_callback(SELINUX_CB_LOG, cb);

    cb.func_audit = audit_callback;
    selinux_set_callback(SELINUX_CB_AUDIT, cb);

    // 加载 selinux 配置信息 
    // se是对rc文件的扩展 
    selinux_initialize();
    /* These directories were necessarily created before initial policy load
     * and therefore need their security context restored to the proper value.
     * This must happen before /dev is populated by ueventd.
     */
    restorecon("/dev");
    restorecon("/dev/socket");
    restorecon("/dev/__properties__");
    restorecon_recursive("/sys");

    is_charger = !strcmp(bootmode, "charger");

    INFO("property init\n");
    if (!is_charger)
        property_load_boot_defaults();

    /*
     * 开始解析 init.rc 脚本 (init.rc 是Android自己规定的初始化脚本 system/core/init/readme.txt ) 
     * 脚本包含四个类型声明: 
     *   Actions 
     *   Commands
     *   Services
     *   Options 
     */
     
    // 递归解析rc文件，生成服务列表与动作列表：
    // 动作列表与服务列表会以列表的形式注册到（全局结构体）service_list 与 action_list 中 
    INFO("reading config file\n");
    init_parse_config_file("/init.rc");

    /*
     * action 命令优先顺序：
     *     early-init  --> init --> early-fs --> fs --> post-fs 
     *     --> post-fs-data --> charger --> early-boot --> boot 
     *
     * on early-init; 在初始化早期阶段触发；
     * on init;       在初始化阶段触发；
     * on late-init;  在初始化晚期阶段触发；
     * on boot/charger：            当系统启动/充电时触发，还包含其他情况，此处不一一列举；
     * on property:<key>=<value>:   当属性值满足条件时触发；
     * 
     * 使用命令 getprop | grep init.svc 可以查看当前系统服务状态 (running,stopped,restarting) 
     */

    // 遍历action_list,优先把 action->name 为"early-init"加入到 action_queue 队列 (即在后面循环中优先被执行) 
    action_for_each_trigger("early-init", action_add_queue_tail);

    // 将系统需要启动的服务加入到 action_queue 队列中 
    queue_builtin_action(wait_for_coldboot_done_action, "wait_for_coldboot_done");  // 等冷插拔设备初始化 
    queue_builtin_action(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");
    queue_builtin_action(keychord_init_action, "keychord_init");  // 设备组合键的初始化操作 
    queue_builtin_action(console_init_action, "console_init");  // 显示启动Logo 

    /* execute all the boot actions to get us started */
    // 触发需要执行的 action 
    action_for_each_trigger("init", action_add_queue_tail);

    /* skip mounting filesystems in charger mode */
    if (!is_charger) {
        action_for_each_trigger("early-fs", action_add_queue_tail);
        action_for_each_trigger("fs", action_add_queue_tail);
        action_for_each_trigger("post-fs", action_add_queue_tail);
        action_for_each_trigger("post-fs-data", action_add_queue_tail);
    }

    /* Repeat mix_hwrng_into_linux_rng in case /dev/hw_random or /dev/random
     * wasn't ready immediately after wait_for_coldboot_done
     */
    queue_builtin_action(mix_hwrng_into_linux_rng_action, "mix_hwrng_into_linux_rng");

    queue_builtin_action(property_service_init_action, "property_service_init"); // 启动属性服务 
    queue_builtin_action(signal_init_action, "signal_init");  // 处理子进程(服务)终止,这里服务第一次启动运行 
    queue_builtin_action(check_startup_action, "check_startup");

    // 当处于充电模式，则 charger 加入执行队列；否则 boot 加入队列 
    if (is_charger) { 
        action_for_each_trigger("charger", action_add_queue_tail);
    } else {
        action_for_each_trigger("early-boot", action_add_queue_tail);
        action_for_each_trigger("boot", action_add_queue_tail);
    }

        /* run all property triggers based on current state of the properties */
    queue_builtin_action(queue_property_triggers_action, "queue_property_triggers"); // 属性触发器 


#if BOOTCHART
    queue_builtin_action(bootchart_init_action, "bootchart_init");
#endif

    // 进入无限循环，建立init的子进程（init是所有进程的父进程） 
    for(;;) {
        int nr, i, timeout = -1;

        /* 
         * 判断action列表是否为空，
         * 如果不为空则，从 action_queue 列表上移除头结点(action),
         * 并执行摘取的 action 命令（子进程对应的命令） 
         */  
        execute_one_command();
        
        /*
         * 遍历 service_list 列表，
         * 如果列表中服务 flags 标记为 SVC_RESTARTING(进程已经死去标志), 
         * 则重新启动该 action 命令
         */
        restart_processes();

        // 注册init进程监控的文件描述符 
        // property service 功能 
        if (!property_set_fd_init && get_property_set_fd() > 0) {
            ufds[fd_count].fd = get_property_set_fd();
            ufds[fd_count].events = POLLIN;
            ufds[fd_count].revents = 0;
            fd_count++;
            property_set_fd_init = 1;
        }
        // 子进程结束signal信号接收服务 
        if (!signal_fd_init && get_signal_fd() > 0) {
            ufds[fd_count].fd = get_signal_fd();
            ufds[fd_count].events = POLLIN;
            ufds[fd_count].revents = 0;
            fd_count++;
            signal_fd_init = 1;
        }
        // 
        if (!keychord_fd_init && get_keychord_fd() > 0) {
            ufds[fd_count].fd = get_keychord_fd();
            ufds[fd_count].events = POLLIN;
            ufds[fd_count].revents = 0;
            fd_count++;
            keychord_fd_init = 1;
        }

        if (process_needs_restart) {
            timeout = (process_needs_restart - gettime()) * 1000;
            if (timeout < 0)
                timeout = 0;
        }

        if (!action_queue_empty() || cur_action)
            timeout = 0;

#if BOOTCHART
        if (bootchart_count > 0) {
            if (timeout < 0 || timeout > BOOTCHART_POLLING_MS)
                timeout = BOOTCHART_POLLING_MS;
            if (bootchart_step() < 0 || --bootchart_count == 0) {
                bootchart_finish();
                bootchart_count = 0;
            }
        }
#endif

        // 热插拔设备监控(监控来自驱动的uevent) 
        nr = poll(ufds, fd_count, timeout);
        if (nr <= 0)
            continue;

        for (i = 0; i < fd_count; i++) {
            if (ufds[i].revents == POLLIN) {
                if (ufds[i].fd == get_property_set_fd())
                    handle_property_set_fd();
                else if (ufds[i].fd == get_keychord_fd())
                    handle_keychord();
                else if (ufds[i].fd == get_signal_fd())
                    handle_signal();
            }
        }
    }

    return 0;
}
