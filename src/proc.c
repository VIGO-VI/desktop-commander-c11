/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Process tools. A session is one child in its own session leader group, with
 * stdout and stderr on one pipe, a reader thread and a reaper that collects
 * the exit status. This is the part Linux makes easy: fork, pipe, poll and
 * waitpid (-1), plus /proc for the process list.
 */
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define DEFAULT_WAIT_MS 1000
#define MAX_WAIT_MS 50000
#define MAX_BUFFER_BYTES (50u * 1024 * 1024)
#define INITIAL_OUTPUT_BYTES (2u * 1024 * 1024)

typedef struct psess {
    pid_t pid;
    char *command;
    char *shell;
    int in_fd;
    int out_fd;
    pthread_t reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    char **lines;
    size_t n, cap;
    size_t bytes;
    size_t evicted;
    size_t unread;   /* index of the first line the caller has not read */
    size_t unread_shift; /* evicted lines already counted in unread */

    bool eof;
    bool reaped;
    int wait_status;
    bool killed;
    long long start_ms;
    long long end_ms;
    struct psess *next;
} psess;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static psess *g_sessions;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void *reaper_main(void *arg);

static void start_reaper(void)
{
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, reaper_main, NULL) != 0)
        fputs("dcm: cannot start the reaper thread\n", stderr);
    pthread_attr_destroy(&attr);
}

static void ensure_init(void)
{
    pthread_once(&g_init_once, start_reaper);
}

static psess *find_session(pid_t pid)
{
    for (psess *s = g_sessions; s; s = s->next)
        if (s->pid == pid)
            return s;
    return NULL;
}

static bool session_done(psess *s)
{
    return s->eof && s->reaped;
}

/* ---------------------------------------------------------------- buffer */

static void line_push(psess *s, const char *line, size_t len)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        s->lines = xrealloc(s->lines, cap * sizeof *s->lines);
        s->cap = cap;
    }
    if (len > 1024 * 1024)
        len = 1024 * 1024;
    char *copy = xmalloc(len + 1);
    memcpy(copy, line, len);
    copy[len] = '\0';
    s->lines[s->n++] = copy;
    s->bytes += len;

    while (s->bytes > MAX_BUFFER_BYTES && s->n > 1) {
        s->bytes -= strlen(s->lines[0]);
        free(s->lines[0]);
        memmove(&s->lines[0], &s->lines[1], (s->n - 1) * sizeof *s->lines);
        s->n--;
        s->evicted++;
        if (s->unread > 0)
            s->unread--;
        else
            s->unread_shift++;
    }
}

/* ---------------------------------------------------------------- reader */

static void *reader_main(void *arg)
{
    psess *s = arg;
    char buf[65536];
    sbuf partial;
    sb_init(&partial);

    for (;;) {
        ssize_t n = read(s->out_fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;

        pthread_mutex_lock(&s->lock);
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                size_t len = partial.n;
                if (len && partial.p[len - 1] == '\r')
                    len--;
                line_push(s, partial.p ? partial.p : "", len);
                sb_free(&partial);
                sb_init(&partial);
            } else {
                sb_putc(&partial, buf[i]);
            }
        }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }

    pthread_mutex_lock(&s->lock);
    if (partial.n > 0) {
        size_t len = partial.n;
        if (partial.p[len - 1] == '\r')
            len--;
        line_push(s, partial.p, len);
    }
    s->eof = true;
    if (s->end_ms == 0)
        s->end_ms = now_ms();
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    sb_free(&partial);
    return NULL;
}

/* ---------------------------------------------------------------- reaper */

static void *reaper_main(void *arg)
{
    (void)arg;
#ifdef __linux__
    /* Orphans of a killed session come back to us instead of init, so the
     * waitpid below collects them and no zombie outlives its session. */
    prctl(PR_SET_CHILD_SUBREAPER, 1);
#endif
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid <= 0) {
            if (errno == EINTR)
                continue;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        for (psess *s = g_sessions; s; s = s->next) {
            if (s->pid == pid) {
                pthread_mutex_lock(&s->lock);
                s->reaped = true;
                s->wait_status = status;
                if (s->end_ms == 0)
                    s->end_ms = now_ms();
                pthread_cond_broadcast(&s->cond);
                pthread_mutex_unlock(&s->lock);
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- spawn */

static char *exit_text(const psess *s)
{
    if (WIFEXITED(s->wait_status))
        return xasprintf("exit code %d", WEXITSTATUS(s->wait_status));
    if (WIFSIGNALED(s->wait_status)) {
        int sig = WTERMSIG(s->wait_status);
        return xasprintf("signal %d (%s)", sig, strsignal(sig));
    }
    return xstrdup("unknown state");
}

static double runtime_s(const psess *s)
{
    long long end = s->end_ms ? s->end_ms : now_ms();
    return (double)(end - s->start_ms) / 1000.0;
}

/* Read lines [from, from+count) under the session lock. */
static void read_page(psess *s, long long offset, long long length, sbuf *out,
                      size_t *count_out, size_t *from_out, size_t *total_out,
                      size_t *remaining_out, bool *tail_out, size_t *evicted_out)
{
    *count_out = 0;
    *from_out = 0;
    *total_out = s->n;
    *remaining_out = 0;
    *tail_out = false;
    *evicted_out = s->evicted;

    size_t from = 0;
    size_t count = 0;
    if (offset < 0) {
        *tail_out = true;
        size_t want = (size_t)(-offset);
        if (want > s->n)
            want = s->n;
        from = s->n - want;
        count = want;
    } else if (offset == 0) {
        from = s->unread;
        size_t avail = s->n > from ? s->n - from : 0;
        count = (size_t)length < avail ? (size_t)length : avail;
        s->unread = from + count;
    } else {
        from = (size_t)offset < s->n ? (size_t)offset : s->n;
        size_t avail = s->n - from;
        count = (size_t)length < avail ? (size_t)length : avail;
    }
    *from_out = from;
    *count_out = count;
    *remaining_out = s->n > from + count ? s->n - (from + count) : 0;
    for (size_t i = from; i < from + count; i++) {
        if (i > from)
            sb_putc(out, '\n');
        sb_put(out, s->lines[i]);
    }
}

/* Wait until the session is done or new unread output exists. */
static void wait_for_output(psess *s, long long ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->lock);
    while (!session_done(s) && s->unread >= s->n) {
        if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&s->lock);
}

/* ---------------------------------------------------------------- tools */

tool_result tool_start_process(json *a)
{
    const char *command = jstr_get(a, "command", NULL);
    if (!command || !*command)
        return result_error("Missing command");

    ensure_init();

    config *cfg = config_load();
    if (command_is_blocked(cfg, command)) {
        config_free(cfg);
        return result_error("Command not allowed: %s", command);
    }
    const char *shell = jstr_get(a, "shell", NULL);
    if (!shell || !*shell)
        shell = cfg->default_shell;
    char *shell_copy = xstrdup(shell);
    char *shell_name = xstrdup(shell);
    config_free(cfg);

    long long timeout = jint_get(a, "timeout_ms", DEFAULT_WAIT_MS);
    if (timeout < 0)
        timeout = 0;
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;

    int in_pipe[2], out_pipe[2];
    /* Linux closes the ends on exec, so a child never holds another session's
     * pipe. The two-way fallback keeps the other Unix systems building. */
#ifdef __linux__
    if (pipe2(in_pipe, O_CLOEXEC) != 0)
        return result_error("Cannot create the input pipe: %s", strerror(errno));
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return result_error("Cannot create the output pipe: %s", strerror(errno));
    }
    fcntl(in_pipe[0], F_SETFD, FD_CLOEXEC);
#else
    if (pipe(in_pipe) != 0)
        return result_error("Cannot create the input pipe: %s", strerror(errno));
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return result_error("Cannot create the output pipe: %s", strerror(errno));
    }
#endif

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        free(shell_copy);
        return result_error("Cannot fork: %s", strerror(errno));
    }

    if (pid == 0) {
        setsid();
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);

        const char *term = "xterm-256color";
        setenv("TERM", term, 1);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);

        const char *base = base_name(shell_copy);
        if (strstr(base, "bash") || strstr(base, "zsh")) {
            execl(shell_copy, shell_copy, "-l", "-c", command, (char *)NULL);
        } else {
            execl(shell_copy, shell_copy, "-c", command, (char *)NULL);
        }
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    free(shell_copy);

    psess *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->pid = pid;
    s->in_fd = in_pipe[1];
    s->out_fd = out_pipe[0];
    s->command = xstrdup(command);
    s->start_ms = now_ms();
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);

    pthread_mutex_lock(&g_lock);
    s->next = g_sessions;
    g_sessions = s;
    pthread_mutex_unlock(&g_lock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&s->reader, &attr, reader_main, s);
    pthread_attr_destroy(&attr);

    /* Collect the first output, up to the timeout or the initial buffer cap. */
    long long deadline = now_ms() + timeout;
    pthread_mutex_lock(&s->lock);
    while (!session_done(s) && s->bytes < INITIAL_OUTPUT_BYTES) {
        long long left = deadline - now_ms();
        if (left <= 0)
            break;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += left / 1000;
        until.tv_nsec += (left % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
            break;
    }
    bool done = session_done(s);

    sbuf out;
    sb_init(&out);
    size_t total = s->n;
    for (size_t i = 0; i < total; i++) {
        if (i)
            sb_putc(&out, '\n');
        sb_put(&out, s->lines[i]);
    }
    char *ending = done ? exit_text(s) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    sb_printf(&text, "Process started with PID %d (shell: %s)\nInitial output:\n%s",
              (int)pid, shell_name, out.p ? out.p : "");
    if (done && ending) {
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", ending, runtime);
    } else {
        sb_put(&text, "\n⏳ Process is running. Use read_process_output to get more output.");
    }
    sb_free(&out);

    json *structured = jobj();
    jobj_set(structured, "pid", jint(pid));
    jobj_set(structured, "shell", jstr(shell_name));
    jobj_set(structured, "status", jstr(done ? "finished" : "running"));
    jobj_set(structured, "waitCapped", jbool(false));

    tool_result r = result_text_struct(text.p, structured);
    sb_free(&text);
    free(ending);
    free(shell_name);
    return r;
}

tool_result tool_read_process_output(json *a)
{
    pid_t pid = (pid_t)jint_get(a, "pid", -1);
    if (pid <= 0)
        return result_error("pid must be a positive number");
    ensure_init();

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (!s)
        return result_error("No session found for PID %d", (int)pid);

    config *cfg = config_load();
    long long length = jint_get(a, "length", cfg->read_line_limit);
    config_free(cfg);
    long long offset = jint_get(a, "offset", 0);
    long long timeout = jint_get(a, "timeout_ms", 5000);
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;

    if (offset == 0 && timeout > 0)
        wait_for_output(s, timeout);

    sbuf out;
    sb_init(&out);
    size_t count = 0, from = 0, total = 0, remaining = 0, evicted = 0;
    bool tail = false;

    pthread_mutex_lock(&s->lock);
    read_page(s, offset, length, &out, &count, &from, &total, &remaining,
              &tail, &evicted);
    bool done = session_done(s);
    char *ending = done ? exit_text(s) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    if (tail)
        sb_printf(&text, "[Reading last %zu lines (total: %zu lines)]", count, total);
    else if (offset == 0) {
        if (remaining > 0)
            sb_printf(&text, "[Reading %zu new lines from line %zu (total: %zu lines, %zu remaining)]",
                      count, from + evicted, total, remaining);
        else
            sb_printf(&text, "[Reading %zu new lines (total: %zu lines)]", count, total);
    } else {
        sb_printf(&text, "[Reading %zu lines from line %zu (total: %zu lines, %zu remaining)]",
                  count, from + evicted, total, remaining);
    }
    if (evicted)
        sb_printf(&text,
                  "\n[WARNING: the output buffer cap evicted %zu early lines; "
                  "line numbers and totals cover the retained buffer only]", evicted);
    sb_put(&text, "\n\n");
    sb_put(&text, out.n ? out.p : "(No output in requested range)");
    if (done && ending)
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", ending, runtime);

    tool_result r = result_text(text.p);
    sb_free(&text);
    sb_free(&out);
    free(ending);
    return r;
}

tool_result tool_interact_with_process(json *a)
{
    pid_t pid = (pid_t)jint_get(a, "pid", -1);
    const char *input = jstr_get(a, "input", NULL);
    if (pid <= 0)
        return result_error("pid must be a positive number");
    if (input == NULL)
        return result_error("Missing input");

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (!s)
        return result_error("No session found for PID %d", (int)pid);

    size_t before = 0;
    pthread_mutex_lock(&s->lock);
    before = s->n;
    pthread_mutex_unlock(&s->lock);

    char *payload;
    if (*input && input[strlen(input) - 1] == '\n')
        payload = xstrdup(input);
    else
        payload = xasprintf("%s\n", input);

    size_t off = 0, len = strlen(payload);
    bool sent = true;
    while (off < len) {
        ssize_t w = write(s->in_fd, payload + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            sent = false;
            break;
        }
        off += (size_t)w;
    }
    free(payload);

    if (!sent)
        return result_error("Failed to send input to process %d. It may have exited.",
                            (int)pid);

    long long timeout = jint_get(a, "timeout_ms", 5000);
    bool wait_prompt = jbool_get(a, "wait_for_prompt", true);
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;
    if (wait_prompt && timeout > 0)
        wait_for_output(s, timeout);

    sbuf out;
    sb_init(&out);
    size_t count = 0, from = 0, remaining = 0, evicted = 0;
    pthread_mutex_lock(&s->lock);
    if (s->n > before) {
        size_t length = s->n - before;
        for (size_t i = before; i < s->n; i++) {
            if (i > before)
                sb_putc(&out, '\n');
            sb_put(&out, s->lines[i]);
        }
        count = length;
        s->unread = s->n;
    }
    bool done = session_done(s);
    char *ending = done ? exit_text(s) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);
    (void)from;
    (void)remaining;
    (void)evicted;

    sbuf text;
    sb_init(&text);
    if (out.n) {
        sb_printf(&text, "[%zu new lines]\n\n%s", count, out.p);
    } else {
        sb_printf(&text, "✅ Input sent to process %d. No new output within %lld ms.",
                  (int)pid, timeout);
    }
    if (done && ending)
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", ending, runtime);

    tool_result r = result_text(text.p);
    sb_free(&text);
    sb_free(&out);
    free(ending);
    return r;
}

tool_result tool_force_terminate(json *a)
{
    pid_t pid = (pid_t)jint_get(a, "pid", -1);
    if (pid <= 0)
        return result_error("pid must be a positive number");
    ensure_init();

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (!s) {
        char *m = xasprintf("No active session found for PID %d", (int)pid);
        tool_result r = result_text(m);
        free(m);
        return r;
    }

    pthread_mutex_lock(&s->lock);
    bool done = session_done(s);
    pthread_mutex_unlock(&s->lock);
    if (done) {
        char *m = xasprintf("No active session found for PID %d", (int)pid);
        tool_result r = result_text(m);
        free(m);
        return r;
    }

    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&s->lock);
        bool fin = session_done(s);
        pthread_mutex_unlock(&s->lock);
        if (fin)
            break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&s->lock);
    bool fin = session_done(s);
    s->killed = true;
    pthread_mutex_unlock(&s->lock);
    if (!fin) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    close(s->in_fd);
    s->in_fd = -1;

    char *msg = xasprintf("Successfully initiated termination of session %d", (int)pid);
    tool_result r = result_text(msg);
    free(msg);
    return r;
}

tool_result tool_list_sessions(json *a)
{
    (void)a;
    ensure_init();
    pthread_mutex_lock(&g_lock);
    sbuf b;
    sb_init(&b);
    size_t count = 0;
    json *list = jarr();
    for (psess *s = g_sessions; s; s = s->next) {
        pthread_mutex_lock(&s->lock);
        bool done = session_done(s);
        if (!done) {
            if (count++)
                sb_putc(&b, '\n');
            sb_printf(&b, "PID: %d, Blocked: false, Runtime: %.0fs",
                      (int)s->pid, runtime_s(s));
            json *item = jobj();
            jobj_set(item, "pid", jint(s->pid));
            jobj_set(item, "type", jstr("process"));
            jobj_set(item, "isBlocked", jbool(false));
            jobj_set(item, "runtimeMs", jint((long long)(runtime_s(s) * 1000)));
            jarr_push(list, item);
        }
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&g_lock);

    if (count == 0)
        return result_text_struct("No active sessions", list);
    char *text = sb_take(&b);
    tool_result r = result_text_struct(text, list);
    free(text);
    return r;
}

/* ---------------------------------------------------------------- ps */

static void ps_line(const char *pid, const char *comm, const char *cpu,
                    const char *mem, sbuf *out)
{
    sb_printf(out, "PID: %s, Command: %s, CPU: %s, Memory: %s\n", pid, comm, cpu, mem);
}

tool_result tool_list_processes(json *a)
{
    (void)a;
    sbuf out;
    sb_init(&out);

#ifdef __linux__
    /* /proc is the kernel's own table: no fork, no ps. */
    DIR *d = opendir("/proc");
    if (!d)
        return result_error("Cannot open /proc: %s", strerror(errno));

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0)
            continue;

        char *comm_path = xasprintf("/proc/%ld/comm", pid);
        size_t clen = 0;
        char *cerr = NULL;
        char *comm = read_file_all(comm_path, &clen, &cerr);
        free(cerr);
        free(comm_path);
        if (!comm) {
            /* The process ended between readdir and here. */
            continue;
        }
        while (clen && (comm[clen - 1] == '\n' || comm[clen - 1] == '\r'))
            comm[--clen] = '\0';

        char *stat_path = xasprintf("/proc/%ld/stat", pid);
        size_t slen = 0;
        char *serr = NULL;
        char *stat = read_file_all(stat_path, &slen, &serr);
        free(serr);
        free(stat_path);
        char state = '?';
        char *comm_field_end = stat ? strrchr(stat, ')') : NULL;
        if (comm_field_end && comm_field_end[1] == ' ' && comm_field_end[2])
            state = comm_field_end[2];

        char pidbuf[32], membuf[32];
        snprintf(pidbuf, sizeof pidbuf, "%ld", pid);
        snprintf(membuf, sizeof membuf, "%c", state);
        char *line = xasprintf("PID: %s, Command: %s, CPU: n/a, Memory: state %s", pidbuf, comm, membuf);
        sb_put(&out, line);
        sb_putc(&out, '\n');
        free(line);
        free(stat);
        free(comm);
    }
    closedir(d);
#else
    FILE *p = popen("ps -axo pid=,comm= 2>/dev/null", "r");
    if (!p)
        return result_error("Cannot run ps");
    char line[4096];
    while (fgets(line, sizeof line, p)) {
        char *s = line;
        while (*s == ' ')
            s++;
        char *space = strchr(s, ' ');
        if (!space)
            continue;
        *space = '\0';
        char *cmd = space + 1;
        char *nl = strchr(cmd, '\n');
        if (nl)
            *nl = '\0';
        ps_line(s, cmd, "n/a", "n/a", &out);
    }
    pclose(p);
#endif

    if (out.n == 0) {
        sb_free(&out);
        return result_text("No processes found.");
    }
    char *text = sb_take(&out);
    tool_result r = result_text(text);
    free(text);
    return r;
}

tool_result tool_kill_process(json *a)
{
    pid_t pid = (pid_t)jint_get(a, "pid", -1);
    if (pid <= 0)
        return result_error("pid must be greater than 0");
    if (pid == getpid())
        return result_error("Refusing to kill the server process");

    /* A session ends with every process its command started. */
    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (s) {
        pthread_mutex_lock(&s->lock);
        bool done = session_done(s);
        pthread_mutex_unlock(&s->lock);
        if (!done) {
            kill(-pid, SIGTERM);
            kill(pid, SIGTERM);
        }
    }

    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        char *msg = xasprintf("Failed to kill process %d: %s", (int)pid, strerror(errno));
        tool_result r = result_error("%s", msg);
        free(msg);
        return r;
    }
    char *msg = xasprintf("Successfully terminated process %d", (int)pid);
    tool_result r = result_text(msg);
    free(msg);
    return r;
}
