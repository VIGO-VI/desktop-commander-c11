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
 *
 * Two rules keep a flood of output from costing the caller tokens and the
 * process its life:
 *
 * - The reader drains the pipe to the end of the process, always. A capped
 *   answer stops the reading of nothing: a command that prints past the cap
 *   reaches its last line instead of dying of SIGPIPE, the way it would if the
 *   read end were closed.
 * - A page comes back as head and tail, and the whole output of the session is
 *   in a file whose path the answer names. The tail carries the error, the
 *   head carries the start, and the middle is still one read_file away.
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
/* The unterminated line, held until its newline arrives. A longer line is
 * flushed in pieces, as before. */
#define PARTIAL_BYTES 65536

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

    char partial[PARTIAL_BYTES]; /* the tail without its newline yet */
    size_t plen;
    uint64_t partial_shown;    /* length of the tail already returned */
    uint64_t bytes_total;      /* bytes read from the pipe since the start */

    int spool;                 /* file of the whole output, or -1 */
    char spool_path[PATH_MAX];
    uint64_t spool_bytes;
    bool spool_stopped;        /* the file cap or a write error ended it */

    bool eof, reaped, killed;
    int wait_status;
    int64_t start_ms, end_ms;
} psess;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static psess g_procs[DCM_MAX_SESSIONS];
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void *reaper_main(void *arg);
static uint64_t retained_lines(const psess *s);

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

/* Writes n bytes, and returns false when the target stops taking them. */
static bool write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (w == 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Milliseconds on the monotonic clock: a wall-clock jump neither stretches a
 * wait nor cuts it short. */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits up to ms on a condition variable, on the monotonic clock. Darwin
 * takes a relative time; Linux takes an absolute one on the clock the
 * condition variable counts on. */
static int cond_wait_ms(pthread_cond_t *cond, pthread_mutex_t *lock, int64_t ms)
{
    if (ms < 0)
        ms = 0;
    struct timespec t;
#ifdef __APPLE__
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    return pthread_cond_timedwait_relative_np(cond, lock, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, lock, &t);
#endif
}

/* A condition variable whose timed wait counts on the monotonic clock. */
static void cond_init(psess *s)
{
#ifndef __APPLE__
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&s->cond, &attr);
    pthread_condattr_destroy(&attr);
#else
    pthread_cond_init(&s->cond, NULL);
#endif
}

/* Lines a session keeps before it starts its output file. */
static uint64_t spool_after(void)
{
    return DCM_OUTPUT_LINES > 0 ? (uint64_t)DCM_OUTPUT_LINES : 1;
}

/* Writes the retained ring to fd, in the two pieces a ring can have. */
static bool write_ring(const psess *s, int fd)
{
    if (s->bytes == 0)
        return true;
    uint32_t room = DCM_SESSION_BYTES - s->rpos;
    size_t first = s->bytes < room ? s->bytes : room;
    if (!write_all(fd, s->buf + s->rpos, first))
        return false;
    return s->bytes == first || write_all(fd, s->buf, s->bytes - first);
}

/* Opens the file that holds the whole output and writes what the ring still
 * keeps, so the file starts at line one even when it opens at the cap. The
 * name carries both PIDs, and a name that is already taken gets a number.
 * Caller holds the session lock. */
static void spool_open(psess *s)
{
    if (s->spool >= 0 || s->spool_stopped || s->spool_path[0])
        return;

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    size_t tlen = strlen(tmp);
    bool slash = tmp[tlen - 1] == '/';

    int fd = -1;
    for (unsigned attempt = 0; attempt < 10 && fd < 0; attempt++) {
        if (attempt == 0)
            snprintf(s->spool_path, sizeof s->spool_path, "%s%sdcm-%d-%d.log", tmp,
                     slash ? "" : "/", (int)getpid(), (int)s->pid);
        else
            snprintf(s->spool_path, sizeof s->spool_path, "%s%sdcm-%d-%d-%u.log", tmp,
                     slash ? "" : "/", (int)getpid(), (int)s->pid, attempt);
        fd = open(s->spool_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) {
        s->spool_path[0] = '\0';
        s->spool_stopped = true;
        return;
    }
    s->spool = fd;
    s->spool_bytes = s->bytes;
    if (!write_ring(s, fd))
        s->spool_stopped = true;
}

/* Appends a whole line to the output file, up to DCM_SPOOL_BYTES. Caller
 * holds the session lock. */
static void spool_append(psess *s, const char *line, size_t len)
{
    if (s->spool < 0 || s->spool_stopped)
        return;
    if (s->spool_bytes + len + 1 > DCM_SPOOL_BYTES) {
        s->spool_stopped = true;
        return;
    }
    if (!write_all(s->spool, line, len) || !write_all(s->spool, "\n", 1)) {
        s->spool_stopped = true;
        close(s->spool);
        s->spool = -1;
        return;
    }
    s->spool_bytes += len + 1;
}

/* Closes the file of a finished session. The index stays in spool_path, so a
 * later answer can still name it. Caller holds the session lock. */
static void spool_close(psess *s)
{
    if (s->spool >= 0) {
        close(s->spool);
        s->spool = -1;
    }
}

/* Ends the file of a session whose slot is taken again: its ring goes at the
 * same time, so the file has no reader left. Caller holds the session lock. */
static void spool_reset(psess *s)
{
    spool_close(s);
    if (s->spool_path[0]) {
        unlink(s->spool_path);
        s->spool_path[0] = '\0';
    }
    s->spool_bytes = 0;
    s->spool_stopped = false;
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

    /* A short command leaves no file; a flood gets one that starts at its
     * first line, because the ring is still small when the cap is passed. */
    if (s->spool < 0 && !s->spool_stopped && !s->spool_path[0] &&
        s->lines_total > spool_after()) {
        spool_open(s);
    } else {
        spool_append(s, line, len);
    }
}

/* ---------------------------------------------------------------- reader */

/* Pushes the unterminated tail as a whole line. When the caller already has it
 * whole (it did not grow since the answer that returned it), the unread
 * pointer moves past it, so the same text is not returned twice. Caller holds
 * the session lock. */
static void push_tail(psess *s)
{
    size_t len = s->plen;
    if (len && s->partial[len - 1] == '\r')
        len--;
    uint64_t idx = s->lines_evicted + retained_lines(s);
    bool whole = s->plen > 0 && s->plen == s->partial_shown;
    line_push(s, s->partial, len);
    if (whole && s->unread >= idx)
        s->unread = idx + 1;
    s->plen = 0;
    s->partial_shown = 0;
}

static void *reader_main(void *arg)
{
    psess *s = arg;
    char buf[65536];

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
        s->bytes_total += (uint64_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                push_tail(s);
            } else if (s->plen < sizeof s->partial) {
                s->partial[s->plen++] = buf[i];
            } else {
                /* A line longer than the buffer: flush it as one. */
                line_push(s, s->partial, s->plen);
                s->plen = 0;
                s->partial[s->plen++] = buf[i];
            }
        }
        /* A byte that is not a whole line yet is still progress, and a waiter
         * must see it: a prompt has no newline. */
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }

    pthread_mutex_lock(&s->lock);
    if (s->plen)
        push_tail(s);
    /* Both ends close here, under the lock, before the slot can be taken
     * again: a reused descriptor number must not be closed by this thread. */
    if (s->out_fd >= 0) {
        close(s->out_fd);
        s->out_fd = -1;
    }
    if (s->in_fd >= 0) {
        close(s->in_fd);
        s->in_fd = -1;
    }
    spool_close(s);
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
        pos = ring_next(pos, 1);
        n++;
    }
    return pos;
}

static uint64_t retained_lines(const psess *s)
{
    return s->lines_total - s->lines_evicted;
}

/* The lines a reader can see: the whole lines of the ring, and the tail that
 * has no newline yet. Caller holds the session lock. */
static uint64_t visible_lines(const psess *s)
{
    return retained_lines(s) + (s->plen ? 1u : 0u);
}

/* Writes visible lines [from, from+count) to out, each with its newline. The
 * unterminated tail is line retained_lines(). Caller holds the lock. */
static void write_page(const psess *s, uint64_t from, uint64_t count, sbuf *out)
{
    uint64_t complete = retained_lines(s);
    uint32_t pos = line_pos(s, from);
    for (uint64_t i = from; i < from + count; i++) {
        if (i >= complete) {
            sb_putn(out, s->partial, s->plen);
            sb_putc(out, '\n');
            continue;
        }
        uint32_t start = pos, len = 0;
        while (len < s->bytes && s->buf[pos] != '\n') {
            pos = ring_next(pos, 1);
            len++;
        }
        ring_copy(s, start, len, out);
        sb_putc(out, '\n');
        pos = ring_next(pos, 1);
    }
}

/* Head and tail of a page, with one line that says how many lines are left out
 * and where the whole output is. A negative max keeps every line. Returns the
 * number of lines written, the marker line excluded. */
static uint64_t write_capped(const psess *s, uint64_t from, uint64_t count, int64_t max,
                             sbuf *out)
{
    if (max < 0 || (uint64_t)max >= count) {
        write_page(s, from, count, out);
        return count;
    }
    uint64_t head = (uint64_t)max / 2;
    uint64_t tail = (uint64_t)max - head;
    uint64_t omitted = count - head - tail;
    if (head)
        write_page(s, from, head, out);
    sb_printf(out, "[... %llu of %llu lines omitted",
              (unsigned long long)omitted, (unsigned long long)count);
    if (s->spool_path[0]) {
        if (s->spool_stopped)
            sb_printf(out, "; the output up to the file cap of %u bytes is in %s",
                      (unsigned)DCM_SPOOL_BYTES, s->spool_path);
        else
            sb_printf(out, "; the whole output is in %s", s->spool_path);
    }
    sb_put(out, " ...]\n");
    if (tail)
        write_page(s, from + count - tail, tail, out);
    return head + tail;
}

typedef struct {
    uint64_t from, count, total, remaining, evicted;
    bool tail;    /* the page was asked for from the end */
    bool partial; /* the page ends with the unterminated tail */
} page;

/* Reads a page and, for offset 0, moves the unread pointer. Caller holds the
 * session lock. */
static void read_page(psess *s, int64_t offset, int64_t length, sbuf *out, page *p)
{
    uint64_t complete = retained_lines(s);
    uint64_t total = visible_lines(s);
    uint64_t from = 0, count = 0;
    bool tail = offset < 0;
    bool at_end = true;

    p->evicted = s->lines_evicted;
    p->tail = tail;
    p->partial = false;

    if (offset < 0) {
        uint64_t want = (uint64_t)(-offset);
        if (want > total)
            want = total;
        from = total - want;
        count = want;
    } else if (offset == 0) {
        from = s->unread > s->lines_evicted ? s->unread - s->lines_evicted : 0;
        if (from > complete)
            from = complete;
        uint64_t have = complete - from;
        count = (uint64_t)length < have ? (uint64_t)length : have;
        at_end = count == have;
    } else {
        from = (uint64_t)offset < total ? (uint64_t)offset : total;
        uint64_t have = total - from;
        count = (uint64_t)length < have ? (uint64_t)length : have;
        at_end = count == have;
    }

    if (!at_end) {
        /* a page that stops before the end has no tail to show */
    } else if (tail || offset > 0) {
        /* An explicit range counts the unterminated tail as a line, and a
         * reader who asks again gets it again. */
        if (s->plen && from + count == total) {
            if (count == 0)
                from = complete;
            count++;
            p->partial = true;
        }
    } else if (length > 0 && s->plen > s->partial_shown && from + count == complete) {
        /* An offset-0 read gets the tail once it grew past the length a
         * previous answer returned, so a progress bar of one line does not
         * spin the caller with the same text. */
        if (count == 0)
            from = complete;
        count++;
        p->partial = true;
    }

    p->from = from;
    p->count = count;
    p->total = total;
    p->remaining = total > from + count ? total - (from + count) : 0;
    if (offset == 0 && count) {
        /* unread counts whole lines only: the tail is returned again, whole,
         * once its newline arrives. */
        s->unread = s->lines_evicted + from + count - (p->partial ? 1u : 0u);
        if (p->partial)
            s->partial_shown = s->plen;
    }
    if (count)
        write_page(s, from, count, out);
}

/* True when the session has output that an offset-0 reader has not seen.
 * Caller holds the session lock. */
static bool has_unread(const psess *s)
{
    uint64_t next = s->unread < s->lines_evicted ? s->lines_evicted : s->unread;
    return next < s->lines_total || s->plen > s->partial_shown;
}

/* The reader sets eof before the reaper collects the status. Waiting for the
 * reaper turns a report of "running" into "completed with exit code 0" for a
 * process that is already gone. Caller holds the session lock. */
static bool settle_done(psess *s, int64_t ms)
{
    int64_t deadline = mono_ms() + ms;
    while (s->eof && !s->reaped) {
        int64_t left = deadline - mono_ms();
        if (left <= 0)
            break;
        if (cond_wait_ms(&s->cond, &s->lock, left) == ETIMEDOUT)
            break;
    }
    return session_done(s);
}

/* Waits until the session ends or its output grows past the two counts. The
 * clock is monotonic, and a byte that is not a whole line yet counts: a prompt
 * with no newline must wake the wait, or a REPL looks dead for the timeout. */
static void wait_for_output(psess *s, int64_t ms, uint64_t seen_lines, uint64_t seen_bytes)
{
    if (ms <= 0)
        return;
    int64_t deadline = mono_ms() + ms;
    pthread_mutex_lock(&s->lock);
    while (!session_done(s) && s->lines_total == seen_lines &&
           s->bytes_total == seen_bytes) {
        int64_t left = deadline - mono_ms();
        if (left <= 0)
            break;
        if (cond_wait_ms(&s->cond, &s->lock, left) == ETIMEDOUT)
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

    int64_t max_lines = jint_get(d, a, "max_output_lines", cfg()->output_lines);

    /* Both pipes close on exec, so a child never holds another session's end.
     * fcntl keeps the two-way path of pipe2 for systems that have neither. */
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0)
        return result_error(d, "Cannot create the input pipe: %s", strerror(errno));
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return result_error(d, "Cannot create the output pipe: %s", strerror(errno));
    }
    fcntl(in_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);

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
            cond_init(s);
            s->in_fd = s->out_fd = s->spool = -1;
            s->used = true;
        } else {
            pthread_mutex_lock(&s->lock);
            s->eof = s->reaped = s->killed = s->exited = false;
            s->wait_status = 0;
            s->end_ms = 0;
            s->rpos = s->wpos = s->bytes = 0;
            s->lines_total = s->lines_evicted = s->unread = 0;
            s->plen = 0;
            s->partial_shown = 0;
            s->bytes_total = 0;
            spool_reset(s);
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

    /* Collect the first output: until the process ends or the deadline. The
     * answer is capped below but the reader keeps draining the pipe to the end
     * of the process, so a command that prints past the cap reaches its last
     * line instead of dying of SIGPIPE. */
    int64_t deadline = mono_ms() + timeout;
    pthread_mutex_lock(&s->lock);
    while (!session_done(s)) {
        int64_t left = deadline - mono_ms();
        if (left <= 0)
            break;
        if (cond_wait_ms(&s->cond, &s->lock, left) == ETIMEDOUT)
            break;
    }
    bool done = settle_done(s, 50);
    uint64_t total = visible_lines(s);

    /* The whole output goes to a file before the answer is cut, so the middle
     * is not lost with the lines this answer leaves out. */
    if (max_lines >= 0 && total > (uint64_t)max_lines)
        spool_open(s);

    sbuf out;
    sb_init(&out);
    uint64_t shown = write_capped(s, 0, total, max_lines, &out);

    /* A page that holds everything moves the unread pointer; a capped one
     * moves it past the head, so the next read continues in the middle. */
    uint64_t head = (max_lines >= 0 && total > (uint64_t)max_lines)
                        ? (uint64_t)max_lines / 2
                        : total;
    if (head == total) {
        s->unread = s->lines_total;
        s->partial_shown = s->plen;
    } else {
        s->unread = s->lines_evicted + head;
    }

    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    char file[PATH_MAX];
    snprintf(file, sizeof file, "%s", s->spool_path);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    sb_printf(&text, "Process started with PID %d (shell: %s)\nInitial output:\n",
              (int)pid, shell);
    if (out.n)
        sb_putn(&text, out.p, out.n);
    else
        sb_put(&text, "(no output yet)\n");
    if (done && fin) {
        sb_printf(&text, "✅ Process completed with %s (runtime: %.2fs)", fin, runtime);
    } else {
        sb_put(&text, "⏳ Process is running. Use read_process_output to get more output.");
    }
    sb_free(&out);

    jref structured = jobj(d);
    jobj_set(d, structured, "pid", jint(d, pid));
    jobj_set(d, structured, "shell", jstr(d, shell));
    jobj_set(d, structured, "status", jstr(d, done ? "finished" : "running"));
    jobj_set(d, structured, "waitCapped", jbool(d, !done));
    jobj_set(d, structured, "maxOutputLines", jint(d, max_lines));
    jobj_set(d, structured, "outputLines", jint(d, (int64_t)total));
    jobj_set(d, structured, "shownLines", jint(d, (int64_t)shown));
    if (file[0])
        jobj_set(d, structured, "outputFile", jstr(d, file));

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

    if (offset == 0 && timeout > 0) {
        /* Wait only when nothing is unread: an unread line must not be held
         * back by the timeout of a call that has an answer already. */
        pthread_mutex_lock(&s->lock);
        bool ready = has_unread(s);
        uint64_t seen_lines = s->lines_total;
        uint64_t seen_bytes = s->bytes_total;
        pthread_mutex_unlock(&s->lock);
        if (!ready)
            wait_for_output(s, timeout, seen_lines, seen_bytes);
    }

    sbuf out;
    sb_init(&out);
    page p;

    pthread_mutex_lock(&s->lock);
    if (s->pid != pid) { /* the slot went to another process meanwhile */
        pthread_mutex_unlock(&s->lock);
        sb_free(&out);
        return result_error(d, "No session found for PID %d", (int)pid);
    }
    read_page(s, offset, length, &out, &p);
    bool done = settle_done(s, 50);
    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    if (p.tail)
        sb_printf(&text, "[Reading last %llu lines (total: %llu lines)]",
                  (unsigned long long)p.count, (unsigned long long)p.total);
    else if (offset == 0) {
        if (p.remaining > 0)
            sb_printf(&text,
                      "[Reading %llu new lines from line %llu (total: %llu lines, %llu remaining)]",
                      (unsigned long long)p.count,
                      (unsigned long long)(p.from + p.evicted), (unsigned long long)p.total,
                      (unsigned long long)p.remaining);
        else
            sb_printf(&text, "[Reading %llu new lines (total: %llu lines)]",
                      (unsigned long long)p.count, (unsigned long long)p.total);
    } else {
        sb_printf(&text,
                  "[Reading %llu lines from line %llu (total: %llu lines, %llu remaining)]",
                  (unsigned long long)p.count, (unsigned long long)(p.from + p.evicted),
                  (unsigned long long)p.total, (unsigned long long)p.remaining);
    }
    if (p.evicted)
        sb_printf(&text,
                  "\n[WARNING: the output buffer cap evicted %llu early lines; "
                  "line numbers and totals cover the retained buffer only]",
                  (unsigned long long)p.evicted);
    sb_put(&text, "\n\n");
    if (out.n)
        sb_putn(&text, out.p, out.n);
    else
        sb_put(&text, "(No output in requested range)\n");
    if (done && fin)
        sb_printf(&text, "✅ Process completed with %s (runtime: %.2fs)", fin, runtime);

    tool_result r = result_buf(d, &text);
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
    uint64_t seen_lines = s->lines_total;
    uint64_t seen_bytes = s->bytes_total;
    /* A copy of the descriptor: the reader closes the session's own end when
     * the process exits, and the number could then belong to another session. */
    int in_fd = s->in_fd >= 0 ? dup(s->in_fd) : -1;
    pthread_mutex_unlock(&s->lock);
    if (in_fd < 0)
        return result_error(d, "No session found for PID %d: it has ended.", (int)pid);

    int64_t max_lines = jint_get(d, a, "max_output_lines", cfg()->output_lines);

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
    close(in_fd);
    sb_free(&payload);

    if (!sent)
        return result_error(d, "Failed to send input to process %d. It may have exited.",
                            (int)pid);

    int64_t timeout = jint_get(d, a, "timeout_ms", 5000);
    bool wait_prompt = jbool_get(d, a, "wait_for_prompt", true);
    if (timeout > MAX_WAIT_MS)
        timeout = MAX_WAIT_MS;
    if (wait_prompt)
        wait_for_output(s, timeout, seen_lines, seen_bytes);

    sbuf out;
    sb_init(&out);
    pthread_mutex_lock(&s->lock);
    uint64_t complete = retained_lines(s);
    uint64_t from = before > s->lines_evicted ? before - s->lines_evicted : 0;
    if (from > complete)
        from = complete;
    uint64_t count = complete - from;
    if (s->plen)
        count++; /* the unterminated tail is output too */
    uint64_t total = visible_lines(s);
    if (from + count > total)
        count = total - from;
    if (max_lines >= 0 && count > (uint64_t)max_lines)
        spool_open(s);
    uint64_t shown = write_capped(s, from, count, max_lines, &out);
    /* Move the unread pointer past what the answer holds. A capped answer
     * moves it past the head only, so the middle is still readable. */
    if (shown == count) {
        s->unread = s->lines_total;
        s->partial_shown = s->plen;
    } else {
        s->unread = s->lines_evicted + from + (uint64_t)max_lines / 2;
    }
    bool done = settle_done(s, 50);
    char ending[128];
    const char *fin = done ? exit_text(s, ending, sizeof ending) : NULL;
    double runtime = runtime_s(s);
    pthread_mutex_unlock(&s->lock);

    sbuf text;
    sb_init(&text);
    if (out.n) {
        if (shown < count)
            sb_printf(&text, "[%llu new lines, %llu shown]\n",
                      (unsigned long long)count, (unsigned long long)shown);
        else
            sb_printf(&text, "[%llu new lines]\n", (unsigned long long)count);
        sb_putn(&text, out.p, out.n);
    } else {
        sb_printf(&text, "✅ Input sent to process %d. No new output within %lld ms.\n",
                  (int)pid, (long long)timeout);
    }
    if (done && fin)
        sb_printf(&text, "✅ Process completed with %s (runtime: %.2fs)", fin, runtime);

    tool_result r = result_buf(d, &text);
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
        tool_result r = result_buf(d, &note);
        return r;
    }

    pthread_mutex_lock(&s->lock);
    bool done = session_done(s);
    pthread_mutex_unlock(&s->lock);
    if (done) {
        sb_printf(&note, "No active session found for PID %d", (int)pid);
        tool_result r = result_buf(d, &note);
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
    tool_result r = result_buf(d, &note);
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
    tool_result r = result_buf(d, &out);
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
    tool_result r = result_buf(d, &b);
    return r;
}
