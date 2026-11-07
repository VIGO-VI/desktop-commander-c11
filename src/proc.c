/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Process tools. A session is one child in its own session leader group, with
 * stdout and stderr on one pipe, a reader thread and a reaper that collects the
 * exit status. This is the part Linux makes easy: fork, pipe, poll and
 * waitpid (-1), plus /proc for the process list.
 *
 * Output lives in one byte ring per session: the oldest lines go first, and a
 * new byte never moves the rest. Sessions are slots of a static array, so a
 * process costs its buffer and nothing else.
 */
#include "config.h"
#include "tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#define INITIAL_OUTPUT_BYTES (2u * 1024 * 1024)

typedef struct {
    bool used;
    bool exited; /* the reader has left; the slot may be reused */
    pid_t pid;
    int in_fd, out_fd;
    pthread_t reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    char *buf;                 /* DCM_SESSION_BYTES, a ring of whole lines */
    uint32_t rpos, wpos, bytes;
    uint64_t lines_total;      /* lines written since the process started */
    uint64_t lines_evicted;    /* lines dropped from the front */
    uint64_t unread;           /* absolute index of the first unread line */

    bool eof, reaped, killed;
    int wait_status;
    int64_t start_ms, end_ms;
} psess;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static psess g_procs[DCM_MAX_SESSIONS];
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

/* Caller holds g_lock. */
static psess *find_session(pid_t pid)
{
    for (size_t i = 0; i < DCM_MAX_SESSIONS; i++)
        if (g_procs[i].used && g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

static bool session_done(const psess *s)
{
    return s->eof && s->reaped;
}

/* ---------------------------------------------------------------- ring */

static uint32_t ring_next(uint32_t pos, uint32_t n)
{
    uint32_t cap = DCM_SESSION_BYTES;
    uint32_t room = cap - pos;
    return n < room ? pos + n : n - room;
}

static void ring_copy(const psess *s, uint32_t pos, size_t n, sbuf *out)
{
    uint32_t room = DCM_SESSION_BYTES - pos;
    size_t first = n < room ? n : room;
    sb_putn(out, s->buf + pos, first);
    if (n > first)
        sb_putn(out, s->buf, n - first);
}

/* Drops the line that starts at rpos. Caller holds the session lock. */
static void drop_line(psess *s)
{
    uint32_t pos = s->rpos, n = 0;
    while (n < s->bytes) {
        char c = s->buf[pos];
        pos = pos + 1 == DCM_SESSION_BYTES ? 0 : pos + 1;
        n++;
        if (c == '\n')
            break;
    }
    s->rpos = pos;
    s->bytes -= n;
    s->lines_evicted++;
    if (s->unread < s->lines_evicted)
        s->unread = s->lines_evicted;
}

/* Writes bytes at the head of the ring. Caller holds the session lock. */
static void ring_write(psess *s, const char *p, size_t n)
{
    uint32_t room = DCM_SESSION_BYTES - s->wpos;
    uint32_t first = n < room ? (uint32_t)n : room;
    memcpy(s->buf + s->wpos, p, first);
    if (n > first)
        memcpy(s->buf, p + first, n - first);
    s->wpos = ring_next(s->wpos, (uint32_t)n);
    s->bytes += (uint32_t)n;
}

/* Appends one line and its newline. Caller holds the session lock. */
static void line_push(psess *s, const char *line, size_t len)
{
    if (len > DCM_SESSION_BYTES / 4)
        len = DCM_SESSION_BYTES / 4;
    uint32_t need = (uint32_t)len + 1;
    while (s->bytes + need > DCM_SESSION_BYTES && s->bytes > 0)
        drop_line(s);
    ring_write(s, line, len);
    ring_write(s, "\n", 1);
    s->lines_total++;
}

/* ---------------------------------------------------------------- reader */

static void *reader_main(void *arg)
{
    psess *s = arg;
    char buf[65536];
    char partial[65536];
    size_t plen = 0;

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
                size_t len = plen;
                if (len && partial[len - 1] == '\r')
                    len--;
                line_push(s, partial, len);
                plen = 0;
            } else if (plen < sizeof partial) {
                partial[plen++] = buf[i];
            } else {
                /* A line longer than the stack buffer: flush it as one. */
                line_push(s, partial, plen);
                plen = 0;
                partial[plen++] = buf[i];
            }
        }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }

    pthread_mutex_lock(&s->lock);
    if (plen) {
        size_t len = plen;
        if (partial[len - 1] == '\r')
            len--;
        line_push(s, partial, len);
    }
    s->eof = true;
    if (s->end_ms == 0)
        s->end_ms = now_ms();
    s->exited = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
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
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        psess *s = find_session(pid);
        if (s) {
            pthread_mutex_lock(&s->lock);
            s->reaped = true;
            s->wait_status = status;
            if (s->end_ms == 0)
                s->end_ms = now_ms();
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->lock);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- spawn */

static const char *exit_text(const psess *s, char *buf, size_t n)
{
    if (WIFEXITED(s->wait_status)) {
        snprintf(buf, n, "exit code %d", WEXITSTATUS(s->wait_status));
    } else if (WIFSIGNALED(s->wait_status)) {
        int sig = WTERMSIG(s->wait_status);
        snprintf(buf, n, "signal %d (%s)", sig, strsignal(sig));
    } else {
        snprintf(buf, n, "unknown state");
    }
    return buf;
}

static double runtime_s(const psess *s)
{
    int64_t end = s->end_ms ? s->end_ms : now_ms();
    return (double)(end - s->start_ms) / 1000.0;
}

/* Byte offset of retained line `i`, walking the ring from the front. */
static uint32_t line_pos(const psess *s, uint64_t i)
{
    uint32_t pos = s->rpos;
    uint32_t seen = 0, n = 0;
    while (n < s->bytes && seen < i) {
        if (s->buf[pos] == '\n')
            seen++;
        pos = pos + 1 == DCM_SESSION_BYTES ? 0 : pos + 1;
        n++;
    }
    return pos;
}

/* Writes lines [from, from+count) of the retained buffer into out. The lines
 * are stored with their newline; the caller gets them joined by \n. */
static void ring_page(const psess *s, uint64_t from, uint64_t count, sbuf *out)
{
    uint32_t pos = line_pos(s, from);
    for (uint64_t i = 0; i < count; i++) {
        if (i)
            sb_putc(out, '\n');
        uint32_t start = pos;
        uint32_t walked = 0;
        while (walked < s->bytes && s->buf[pos] != '\n') {
            pos = pos + 1 == DCM_SESSION_BYTES ? 0 : pos + 1;
            walked++;
        }
        uint32_t len = (pos + (pos < start ? DCM_SESSION_BYTES : 0)) - start;
        ring_copy(s, start, len, out);
        pos = pos + 1 == DCM_SESSION_BYTES ? 0 : pos + 1;
    }
}

static uint64_t retained_lines(const psess *s)
{
    return s->lines_total - s->lines_evicted;
}

/* Read lines [from, from+count) under the session lock. */
static void read_page(psess *s, int64_t offset, int64_t length, sbuf *out,
                      uint64_t *count_out, uint64_t *from_out, uint64_t *total_out,
                      uint64_t *remaining_out, bool *tail_out, uint64_t *evicted_out)
{
    uint64_t total = retained_lines(s);
    uint64_t from = 0, count = 0;
    *tail_out = offset < 0;
    *evicted_out = s->lines_evicted;

    if (offset < 0) {
        uint64_t want = (uint64_t)(-offset);
        if (want > total)
            want = total;
        from = total - want;
        count = want;
    } else if (offset == 0) {
        from = s->unread > s->lines_evicted ? s->unread - s->lines_evicted : 0;
        if (from > total)
            from = total;
        uint64_t avail = total - from;
        count = (uint64_t)length < avail ? (uint64_t)length : avail;
        s->unread = s->lines_evicted + from + count;
    } else {
        from = (uint64_t)offset < total ? (uint64_t)offset : total;
        uint64_t avail = total - from;
        count = (uint64_t)length < avail ? (uint64_t)length : avail;
    }
    *from_out = from;
    *count_out = count;
    *total_out = total;
    *remaining_out = total > from + count ? total - (from + count) : 0;
    ring_page(s, from, count, out);
}

/* Wait until the session is done or new unread output exists. */
static void wait_for_output(psess *s, int64_t ms)
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
    while (!session_done(s) && s->unread >= s->lines_total) {
        if (pthread_cond_timedwait(&s->cond, &s->lock, &until) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&s->lock);
}

/* ---------------------------------------------------------------- tools */

/* A free slot, or the oldest finished one. Caller holds g_lock. */
static psess *take_slot(void)
{
    for (size_t i = 0; i < DCM_MAX_SESSIONS; i++)
        if (!g_procs[i].used)
            return &g_procs[i];
    psess *oldest = NULL;
    for (size_t i = 0; i < DCM_MAX_SESSIONS; i++) {
        psess *s = &g_procs[i];
        if (s->exited && (!oldest || s->start_ms < oldest->start_ms))
            oldest = s;
    }
    return oldest;
}

tool_result tool_start_process(jdoc *d, jref a)
{
    const char *command = jstr_get(d, a, "command", NULL);
    if (!command || !*command)
        return result_error(d, "Missing command");

    ensure_init();
    if (command_is_blocked(command))
        return result_error(d, "Command not allowed: %s", command);

    const char *shell = jstr_get(d, a, "shell", NULL);
    if (!shell || !*shell)
        shell = cfg()->shell;

    int64_t timeout = jint_get(d, a, "timeout_ms", DEFAULT_WAIT_MS);
    if (timeout < 0)
        timeout = 0;
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;

    int in_pipe[2], out_pipe[2];
    /* Linux closes the ends on exec, so a child never holds another session's
     * pipe. The two-way fallback keeps the other Unix systems building. */
#ifdef __linux__
    if (pipe2(in_pipe, O_CLOEXEC) != 0)
        return result_error(d, "Cannot create the input pipe: %s", strerror(errno));
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return result_error(d, "Cannot create the output pipe: %s", strerror(errno));
    }
    fcntl(in_pipe[0], F_SETFD, FD_CLOEXEC);
#else
    if (pipe(in_pipe) != 0)
        return result_error(d, "Cannot create the input pipe: %s", strerror(errno));
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return result_error(d, "Cannot create the output pipe: %s", strerror(errno));
    }
#endif

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return result_error(d, "Cannot fork: %s", strerror(errno));
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

        setenv("TERM", "xterm-256color", 1);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);

        const char *base = base_name(shell);
        if (strstr(base, "bash") || strstr(base, "zsh")) {
            execl(shell, shell, "-l", "-c", command, (char *)NULL);
        } else {
            execl(shell, shell, "-c", command, (char *)NULL);
        }
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    pthread_mutex_lock(&g_lock);
    psess *s = take_slot();
    if (s) {
        if (!s->used) {
            memset(s, 0, sizeof *s);
            pthread_mutex_init(&s->lock, NULL);
            pthread_cond_init(&s->cond, NULL);
            s->in_fd = s->out_fd = -1;
            s->used = true;
        } else {
            pthread_mutex_lock(&s->lock);
            s->eof = s->reaped = s->killed = s->exited = false;
            s->wait_status = 0;
            s->end_ms = 0;
            s->rpos = s->wpos = s->bytes = 0;
            s->lines_total = s->lines_evicted = s->unread = 0;
            pthread_mutex_unlock(&s->lock);
        }
        if (!s->buf)
            s->buf = xmalloc(DCM_SESSION_BYTES);
        s->pid = pid;
        s->in_fd = in_pipe[1];
        s->out_fd = out_pipe[0];
        s->start_ms = now_ms();
    }
    pthread_mutex_unlock(&g_lock);

    if (!s) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        return result_error(d, "No free process session: terminate one with force_terminate.");
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&s->reader, &attr, reader_main, s) != 0) {
        pthread_attr_destroy(&attr);
        return result_error(d, "Cannot start the reader thread");
    }
    pthread_attr_destroy(&attr);

    /* Collect the first output, up to the timeout or the initial buffer cap. */
    int64_t deadline = now_ms() + timeout;
    pthread_mutex_lock(&s->lock);
    while (!session_done(s) && s->bytes < INITIAL_OUTPUT_BYTES) {
        int64_t left = deadline - now_ms();
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
    uint64_t total = retained_lines(s);

    sbuf out;
    sb_init(&out);
    ring_page(s, 0, total, &out);
    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    sb_printf(&text, "Process started with PID %d (shell: %s)\nInitial output:\n%s",
              (int)pid, shell, out.p ? out.p : "");
    if (done && fin) {
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", fin, runtime);
    } else {
        sb_put(&text, "\n⏳ Process is running. Use read_process_output to get more output.");
    }
    sb_free(&out);

    jref structured = jobj(d);
    jobj_set(d, structured, "pid", jint(d, pid));
    jobj_set(d, structured, "shell", jstr(d, shell));
    jobj_set(d, structured, "status", jstr(d, done ? "finished" : "running"));
    jobj_set(d, structured, "waitCapped", jbool(d, false));

    tool_result r = result_text_struct(d, text.p ? text.p : "", structured);
    sb_free(&text);
    return r;
}

tool_result tool_read_process_output(jdoc *d, jref a)
{
    pid_t pid = (pid_t)jint_get(d, a, "pid", -1);
    if (pid <= 0)
        return result_error(d, "pid must be a positive number");
    ensure_init();

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (!s)
        return result_error(d, "No session found for PID %d", (int)pid);

    int64_t length = jint_get(d, a, "length", cfg()->read_line_limit);
    int64_t offset = jint_get(d, a, "offset", 0);
    int64_t timeout = jint_get(d, a, "timeout_ms", 5000);
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;

    if (offset == 0 && timeout > 0)
        wait_for_output(s, timeout);

    sbuf out;
    sb_init(&out);
    uint64_t count = 0, from = 0, total = 0, remaining = 0, evicted = 0;
    bool tail = false;

    pthread_mutex_lock(&s->lock);
    if (s->pid != pid) { /* the slot went to another process meanwhile */
        pthread_mutex_unlock(&s->lock);
        sb_free(&out);
        return result_error(d, "No session found for PID %d", (int)pid);
    }
    read_page(s, offset, length, &out, &count, &from, &total, &remaining, &tail, &evicted);
    bool done = session_done(s);
    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    if (tail)
        sb_printf(&text, "[Reading last %llu lines (total: %llu lines)]",
                  (unsigned long long)count, (unsigned long long)total);
    else if (offset == 0) {
        if (remaining > 0)
            sb_printf(&text,
                      "[Reading %llu new lines from line %llu (total: %llu lines, %llu remaining)]",
                      (unsigned long long)count, (unsigned long long)(from + evicted),
                      (unsigned long long)total, (unsigned long long)remaining);
        else
            sb_printf(&text, "[Reading %llu new lines (total: %llu lines)]",
                      (unsigned long long)count, (unsigned long long)total);
    } else {
        sb_printf(&text,
                  "[Reading %llu lines from line %llu (total: %llu lines, %llu remaining)]",
                  (unsigned long long)count, (unsigned long long)(from + evicted),
                  (unsigned long long)total, (unsigned long long)remaining);
    }
    if (evicted)
        sb_printf(&text,
                  "\n[WARNING: the output buffer cap evicted %llu early lines; "
                  "line numbers and totals cover the retained buffer only]",
                  (unsigned long long)evicted);
    sb_put(&text, "\n\n");
    sb_put(&text, out.n ? out.p : "(No output in requested range)");
    if (done && fin)
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", fin, runtime);

    tool_result r = result_text(d, text.p ? text.p : "");
    sb_free(&text);
    sb_free(&out);
    return r;
}

tool_result tool_interact_with_process(jdoc *d, jref a)
{
    pid_t pid = (pid_t)jint_get(d, a, "pid", -1);
    const char *input = jstr_get(d, a, "input", NULL);
    if (pid <= 0)
        return result_error(d, "pid must be a positive number");
    if (input == NULL)
        return result_error(d, "Missing input");

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    if (!s)
        return result_error(d, "No session found for PID %d", (int)pid);

    pthread_mutex_lock(&s->lock);
    if (s->pid != pid) {
        pthread_mutex_unlock(&s->lock);
        return result_error(d, "No session found for PID %d", (int)pid);
    }
    uint64_t before = s->lines_total;
    int in_fd = s->in_fd;
    pthread_mutex_unlock(&s->lock);

    sbuf payload;
    sb_init(&payload);
    size_t ilen = strlen(input);
    sb_putn(&payload, input, ilen);
    if (ilen == 0 || input[ilen - 1] != '\n')
        sb_putc(&payload, '\n');

    size_t off = 0;
    bool sent = true;
    while (off < payload.n) {
        ssize_t w = write(in_fd, payload.p + off, payload.n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            sent = false;
            break;
        }
        off += (size_t)w;
    }
    sb_free(&payload);

    if (!sent)
        return result_error(d, "Failed to send input to process %d. It may have exited.",
                            (int)pid);

    int64_t timeout = jint_get(d, a, "timeout_ms", 5000);
    bool wait_prompt = jbool_get(d, a, "wait_for_prompt", true);
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;
    if (wait_prompt && timeout > 0)
        wait_for_output(s, timeout);

    sbuf out;
    sb_init(&out);
    pthread_mutex_lock(&s->lock);
    uint64_t from = before > s->lines_evicted ? before - s->lines_evicted : 0;
    uint64_t total = retained_lines(s);
    if (from > total)
        from = total;
    uint64_t count = total - from;
    ring_page(s, from, count, &out);
    s->unread = s->lines_total;
    bool done = session_done(s);
    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    if (out.n)
        sb_printf(&text, "[%llu new lines]\n\n%s", (unsigned long long)count, out.p);
    else
        sb_printf(&text, "✅ Input sent to process %d. No new output within %lld ms.",
                  (int)pid, (long long)timeout);
    if (done && fin)
        sb_printf(&text, "\n✅ Process completed with %s (runtime: %.2fs)", fin, runtime);

    tool_result r = result_text(d, text.p ? text.p : "");
    sb_free(&text);
    sb_free(&out);
    return r;
}

tool_result tool_force_terminate(jdoc *d, jref a)
{
    pid_t pid = (pid_t)jint_get(d, a, "pid", -1);
    if (pid <= 0)
        return result_error(d, "pid must be a positive number");
    ensure_init();

    pthread_mutex_lock(&g_lock);
    psess *s = find_session(pid);
    pthread_mutex_unlock(&g_lock);
    sbuf note;
    sb_init(&note);
    if (!s) {
        sb_printf(&note, "No active session found for PID %d", (int)pid);
        tool_result r = result_text(d, note.p ? note.p : "");
        sb_free(&note);
        return r;
    }

    pthread_mutex_lock(&s->lock);
    bool done = session_done(s);
    pthread_mutex_unlock(&s->lock);
    if (done) {
        sb_printf(&note, "No active session found for PID %d", (int)pid);
        tool_result r = result_text(d, note.p ? note.p : "");
        sb_free(&note);
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
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000000L};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&s->lock);
    bool fin = session_done(s);
    s->killed = true;
    int in_fd = s->in_fd;
    s->in_fd = -1;
    pthread_mutex_unlock(&s->lock);
    if (!fin) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    if (in_fd >= 0)
        close(in_fd);

    sb_printf(&note, "Successfully initiated termination of session %d", (int)pid);
    tool_result r = result_text(d, note.p ? note.p : "");
    sb_free(&note);
    return r;
}

tool_result tool_list_sessions(jdoc *d, jref a)
{
    (void)a;
    ensure_init();
    sbuf b;
    sb_init(&b);
    size_t count = 0;
    jref list = jarr(d);

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < DCM_MAX_SESSIONS; i++) {
        psess *s = &g_procs[i];
        if (!s->used)
            continue;
        pthread_mutex_lock(&s->lock);
        if (!session_done(s)) {
            if (count++)
                sb_putc(&b, '\n');
            sb_printf(&b, "PID: %d, Blocked: false, Runtime: %.0fs", (int)s->pid,
                      runtime_s(s));
            jref item = jobj(d);
            jobj_set(d, item, "pid", jint(d, s->pid));
            jobj_set(d, item, "type", jstr(d, "process"));
            jobj_set(d, item, "isBlocked", jbool(d, false));
            jobj_set(d, item, "runtimeMs", jint(d, (int64_t)(runtime_s(s) * 1000)));
            jarr_push(d, list, item);
        }
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&g_lock);

    if (count == 0) {
        sb_free(&b);
        return result_text_struct(d, "No active sessions", list);
    }
    tool_result r = result_text_struct(d, b.p ? b.p : "", list);
    sb_free(&b);
    return r;
}

/* ---------------------------------------------------------------- ps */

#ifdef __linux__
/* Reads a /proc file into buf without allocating; returns its length. */
static size_t read_small(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return (size_t)n;
}
#endif

tool_result tool_list_processes(jdoc *d, jref a)
{
    (void)a;
    sbuf out;
    sb_init(&out);

#ifdef __linux__
    /* /proc is the kernel's own table: no fork, no ps. */
    DIR *dir = opendir("/proc");
    if (!dir) {
        sb_free(&out);
        return result_error(d, "Cannot open /proc: %s", strerror(errno));
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char *end = NULL;
        long pid = strtol(de->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0)
            continue;

        char path[64], comm[256], stat[512];
        snprintf(path, sizeof path, "/proc/%ld/comm", pid);
        size_t clen = read_small(path, comm, sizeof comm);
        if (clen == 0)
            continue; /* the process ended between readdir and here */
        while (clen && (comm[clen - 1] == '\n' || comm[clen - 1] == '\r'))
            comm[--clen] = '\0';

        snprintf(path, sizeof path, "/proc/%ld/stat", pid);
        read_small(path, stat, sizeof stat);
        char state = '?';
        char *field = strrchr(stat, ')');
        if (field && field[1] == ' ' && field[2])
            state = field[2];

        sb_printf(&out, "PID: %ld, Command: %s, CPU: n/a, Memory: state %c\n", pid, comm,
                  state);
    }
    closedir(dir);
#else
    FILE *p = popen("ps -axo pid=,comm= 2>/dev/null", "r");
    if (!p) {
        sb_free(&out);
        return result_error(d, "Cannot run ps");
    }
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
        sb_printf(&out, "PID: %s, Command: %s, CPU: n/a, Memory: n/a\n", s, cmd);
    }
    pclose(p);
#endif

    if (out.n == 0) {
        sb_free(&out);
        return result_text(d, "No processes found.");
    }
    tool_result r = result_text(d, out.p ? out.p : "");
    sb_free(&out);
    return r;
}

tool_result tool_kill_process(jdoc *d, jref a)
{
    pid_t pid = (pid_t)jint_get(d, a, "pid", -1);
    if (pid <= 0)
        return result_error(d, "pid must be greater than 0");
    if (pid == getpid())
        return result_error(d, "Refusing to kill the server process");

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
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "Failed to kill process %d: %s", (int)pid, strerror(errno));
        tool_result r = result_error(d, "%s", b.p ? b.p : "");
        sb_free(&b);
        return r;
    }
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "Successfully terminated process %d", (int)pid);
    tool_result r = result_text(d, b.p ? b.p : "");
    sb_free(&b);
    return r;
}
