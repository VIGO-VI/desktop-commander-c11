/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * MCP server over stdio: one JSON-RPC message per line. The reader hands each
 * line to a fixed pool of workers, so a call that waits on a process does not
 * hold up the next one, and nothing is allocated per request beyond the first
 * use of a worker's document.
 */
#include "config.h"
#include "json.h"
#include "tools.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef tool_result (*tool_fn)(jdoc *d, jref args);

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *schema;
    tool_fn fn;
    bool read_only;
    bool destructive;
    bool open_world;
} tool_def;

/* Short descriptions: each one says what the tool does and the one detail a
 * caller needs. The schema carries the rest. */
static const tool_def TOOLS[] = {
    { "get_config", "Get Configuration",
      "Get the build configuration: blockedCommands, defaultShell, "
      "allowedDirectories, fileReadLineLimit, fileWriteLineLimit, maxOutputLines, "
      "maxOutputBytes, version, systemInfo.",
      "{\"type\":\"object\",\"properties\":{}}", tool_get_config, true, false, false },

    { "set_config_value", "Set Configuration Value",
      "Refused: this build takes its configuration at compile time, in src/config.h.",
      "{\"type\":\"object\",\"properties\":{"
      "\"key\":{\"type\":\"string\"},"
      "\"value\":{\"type\":[\"string\",\"number\",\"boolean\",\"array\",\"null\"],"
      "\"items\":{\"type\":\"string\"}}},"
      "\"required\":[\"key\",\"value\"]}",
      tool_set_config_value, false, true, false },

    { "read_file", "Read File",
      "Read a file as text, or as an image block for PNG/JPEG/GIF/WebP. offset < 0 reads "
      "the last lines; length limits the lines returned.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"isUrl\":{\"type\":\"boolean\",\"default\":false},"
      "\"offset\":{\"type\":\"number\",\"default\":0},"
      "\"length\":{\"type\":\"number\",\"default\":1000},"
      "\"sheet\":{\"type\":\"string\"},\"range\":{\"type\":\"string\"},"
      "\"options\":{\"type\":\"object\"}},"
      "\"required\":[\"path\"]}",
      tool_read_file, true, false, false },

    { "read_multiple_files", "Read Multiple Files",
      "Read several files at once. A failed file is reported and does not stop the rest.",
      "{\"type\":\"object\",\"properties\":{"
      "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
      "\"required\":[\"paths\"]}",
      tool_read_multiple_files, true, false, false },

    { "write_file", "Write File",
      "Write or append to a text file. mode must be given for an existing non-empty file.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"content\":{\"type\":\"string\"},"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"rewrite\",\"append\"],\"default\":\"rewrite\"}},"
      "\"required\":[\"path\",\"content\"]}",
      tool_write_file, false, true, false },

    { "create_directory", "Create Directory",
      "Create a directory and its parents.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}",
      tool_create_directory, false, false, false },

    { "list_directory", "List Directory",
      "List a directory: [DIR] and [FILE] lines, recursive to depth (default 2). "
      "A nested directory shows at most 100 entries and then a warning.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"depth\":{\"type\":\"number\",\"default\":2}},"
      "\"required\":[\"path\"]}",
      tool_list_directory, true, false, false },

    { "move_file", "Move File",
      "Move or rename a file or directory.",
      "{\"type\":\"object\",\"properties\":{"
      "\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},"
      "\"required\":[\"source\",\"destination\"]}",
      tool_move_file, false, true, false },

    { "get_file_info", "Get File Info",
      "Get size, times, permissions and type of a path.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}",
      tool_get_file_info, true, false, false },

    { "edit_block", "Edit Block",
      "Replace exact text in a file, expected_replacements times. When the text is not "
      "found, the closest match and its similarity are reported and nothing is written.",
      "{\"type\":\"object\",\"properties\":{"
      "\"file_path\":{\"type\":\"string\"},"
      "\"old_string\":{\"type\":\"string\"},"
      "\"new_string\":{\"type\":\"string\"},"
      "\"expected_replacements\":{\"type\":\"number\",\"default\":1},"
      "\"range\":{\"type\":\"string\"},"
      "\"content\":{},\"options\":{\"type\":\"object\"}},"
      "\"required\":[\"file_path\"]}",
      tool_edit_block, false, true, false },

    { "start_search", "Start Search",
      "Start a background search and return a session id. searchType files matches names, "
      "content matches text inside files. Read results with get_more_search_results.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"pattern\":{\"type\":\"string\"},"
      "\"searchType\":{\"type\":\"string\",\"enum\":[\"files\",\"content\"],\"default\":\"files\"},"
      "\"filePattern\":{\"type\":\"string\"},"
      "\"ignoreCase\":{\"type\":\"boolean\",\"default\":true},"
      "\"maxResults\":{\"type\":\"number\"},"
      "\"includeHidden\":{\"type\":\"boolean\",\"default\":false},"
      "\"contextLines\":{\"type\":\"number\",\"default\":5},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"earlyTermination\":{\"type\":\"boolean\"},"
      "\"literalSearch\":{\"type\":\"boolean\",\"default\":false}},"
      "\"required\":[\"path\",\"pattern\"]}",
      tool_start_search, true, false, false },

    { "get_more_search_results", "Get More Search Results",
      "Read a page of a search by session id. offset is the row index, length the page size.",
      "{\"type\":\"object\",\"properties\":{"
      "\"sessionId\":{\"type\":\"string\"},"
      "\"offset\":{\"type\":\"number\",\"default\":0},"
      "\"length\":{\"type\":\"number\",\"default\":100}},"
      "\"required\":[\"sessionId\"]}",
      tool_get_more_search_results, true, false, false },

    { "stop_search", "Stop Search",
      "Stop a running search session.",
      "{\"type\":\"object\",\"properties\":{\"sessionId\":{\"type\":\"string\"}},"
      "\"required\":[\"sessionId\"]}",
      tool_stop_search, false, false, false },

    { "list_searches", "List Searches",
      "List search sessions with their state.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_searches, true, false, false },

    { "start_process", "Start Process",
      "Run a shell command and wait up to timeout_ms for output. A command still running "
      "returns a PID for read_process_output and interact_with_process. The answer keeps the "
      "first and last max_output_lines lines, and at most DCM_OUTPUT_BYTES bytes; the whole "
      "output is in the file it names.",
      "{\"type\":\"object\",\"properties\":{"
      "\"command\":{\"type\":\"string\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"max_output_lines\":{\"type\":\"number\",\"default\":200},"
      "\"shell\":{\"type\":\"string\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"command\",\"timeout_ms\"]}",
      tool_start_process, false, true, true },

    { "read_process_output", "Read Process Output",
      "Read a session's output. offset 0 reads new lines, a positive offset reads from that "
      "line, a negative offset reads the last lines. Waits timeout_ms for new output.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pid\":{\"type\":\"number\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"offset\":{\"type\":\"number\"},"
      "\"length\":{\"type\":\"number\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"pid\"]}",
      tool_read_process_output, true, false, false },

    { "interact_with_process", "Interact With Process",
      "Send input to a running session and return its new output, capped to max_output_lines "
      "as head and tail.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pid\":{\"type\":\"number\"},"
      "\"input\":{\"type\":\"string\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"max_output_lines\":{\"type\":\"number\",\"default\":200},"
      "\"wait_for_prompt\":{\"type\":\"boolean\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"pid\",\"input\"]}",
      tool_interact_with_process, false, false, false },

    { "force_terminate", "Force Terminate",
      "End a session and every process its command started.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"number\"}},"
      "\"required\":[\"pid\"]}",
      tool_force_terminate, false, true, false },

    { "list_sessions", "List Sessions",
      "List the running sessions with their PIDs and run time.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_sessions, true, false, false },

    { "list_processes", "List Processes",
      "List the processes of the machine.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_processes, true, false, false },

    { "kill_process", "Kill Process",
      "Terminate one process by PID.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"number\"}},"
      "\"required\":[\"pid\"]}",
      tool_kill_process, false, true, false },

    { "get_usage_stats", "Get Usage Stats",
      "Call counts for this session.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_get_usage_stats, true, false, false },

    { "get_recent_tool_calls", "Get Recent Tool Calls",
      "List recent tool calls from the local history.",
      "{\"type\":\"object\",\"properties\":{"
      "\"maxResults\":{\"type\":\"number\",\"default\":50},"
      "\"toolName\":{\"type\":\"string\"},"
      "\"since\":{\"type\":\"string\"}}}",
      tool_get_recent_tool_calls, true, false, false },
};

#define TOOL_COUNT (sizeof TOOLS / sizeof TOOLS[0])

#define INSTRUCTIONS \
    "Desktop Commander: files, search and long-running terminal sessions on this machine. " \
    "This build has no telemetry."

/* ---------------------------------------------------------------- output */

static pthread_mutex_t g_out = PTHREAD_MUTEX_INITIALIZER;

/* One response is one write, so two workers never interleave a line. */
static void write_out(sbuf *b)
{
    sb_putc(b, '\n');
    pthread_mutex_lock(&g_out);
    if (fwrite(b->p, 1, b->n, stdout) != b->n)
        clearerr(stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_out);
    sb_reset(b);
}

/* ---------------------------------------------------------------- handlers */

static const tool_def *find_tool(const char *name)
{
    for (size_t i = 0; i < TOOL_COUNT; i++)
        if (strcmp(TOOLS[i].name, name) == 0)
            return &TOOLS[i];
    return NULL;
}

static void put_envelope(sbuf *out, jdoc *d, jref id)
{
    sb_put(out, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_dump_into(out, d, id);
}

static void reply_result(sbuf *out, jdoc *d, jref id, jref result)
{
    put_envelope(out, d, id);
    sb_put(out, ",\"result\":");
    json_dump_into(out, d, result);
    sb_putc(out, '}');
    write_out(out);
}

static void reply_error(sbuf *out, jdoc *d, jref id, int code, const char *message)
{
    put_envelope(out, d, id);
    sb_printf(out, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json_str(out, message);
    sb_putc(out, '}');
    sb_putc(out, '}');
    write_out(out);
}

/* The tool list is written straight from the table: the schemas are already
 * JSON text, so nothing is parsed and nothing is copied. */
static void tools_list(sbuf *out)
{
    sb_put(out, "{\"tools\":[");
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        const tool_def *t = &TOOLS[i];
        if (i)
            sb_putc(out, ',');
        sb_put(out, "{\"name\":");
        sb_json_str(out, t->name);
        sb_put(out, ",\"title\":");
        sb_json_str(out, t->title);
        sb_put(out, ",\"description\":");
        sb_json_str(out, t->description);
        sb_printf(out, ",\"inputSchema\":%s,\"annotations\":{\"title\":", t->schema);
        sb_json_str(out, t->title);
        sb_printf(out, ",\"readOnlyHint\":%s,\"destructiveHint\":%s,\"openWorldHint\":%s}}",
                  t->read_only ? "true" : "false", t->destructive ? "true" : "false",
                  t->open_world ? "true" : "false");
    }
    sb_put(out, "]}");
}

static void handle_call(sbuf *out, jdoc *d, jref id, jref params)
{
    const char *name = jstr_get(d, params, "name", NULL);
    const tool_def *t = name ? find_tool(name) : NULL;
    if (!t) {
        reply_error(out, d, id, -32602, "Unknown tool");
        return;
    }
    jref args = jget(d, params, "arguments");
    if (!jis_obj(d, args))
        args = jobj(d);

    tool_result r = t->fn(d, args);
    history_record(d, t->name, args, &r);

    jref result = jobj(d);
    jobj_set(d, result, "content", r.content);
    if (r.is_error)
        jobj_set(d, result, "isError", jbool(d, true));
    if (r.structured != JNULL)
        jobj_set(d, result, "structuredContent", r.structured);
    reply_result(out, d, id, result);
}

/* The protocol versions this build speaks. A client that asks for one of them
 * gets it back; anything else gets the newest, which is what the lifecycle
 * section of the specification asks for. */
static const char *const PROTOCOL_VERSIONS[] = {
    "2025-06-18", "2025-03-26", "2024-11-05", "2024-10-07"
};

static const char *negotiate_version(const char *asked)
{
    if (asked) {
        for (size_t i = 0; i < sizeof PROTOCOL_VERSIONS / sizeof PROTOCOL_VERSIONS[0]; i++)
            if (strcmp(asked, PROTOCOL_VERSIONS[i]) == 0)
                return asked;
    }
    return PROTOCOL_VERSIONS[0];
}

static void handle_initialize(sbuf *out, jdoc *d, jref id, jref params)
{
    const char *asked = jstr_get(d, params, "protocolVersion", NULL);
    jref caps = jobj(d);
    jobj_set(d, caps, "tools", jobj(d));
    jobj_set(d, caps, "resources", jobj(d));
    jobj_set(d, caps, "prompts", jobj(d));
    jobj_set(d, caps, "logging", jobj(d));

    jref server = jobj(d);
    jobj_set(d, server, "name", jstr(d, "desktop-commander"));
    jobj_set(d, server, "version", jstr(d, DCM_VERSION));

    jref result = jobj(d);
    jobj_set(d, result, "protocolVersion", jstr(d, negotiate_version(asked)));
    jobj_set(d, result, "capabilities", caps);
    jobj_set(d, result, "serverInfo", server);
    jobj_set(d, result, "instructions", jstr(d, INSTRUCTIONS));
    reply_result(out, d, id, result);
}

/* True when v can be the id of a request: a string or a number, and not null. */
static bool id_is_valid(jdoc *d, jref v)
{
    jtype t = jtype_of(d, v);
    if (t == J_REAL)
        return isfinite(jnum_of(d, v)); /* a value with no JSON spelling */
    return t == J_STR || t == J_INT;
}

/* One message of the line, already parsed. A message with no id is a
 * notification: it is run and never answered, whatever its method. */
static void dispatch_message(jdoc *d, sbuf *out, jref msg)
{
    if (!jis_obj(d, msg)) {
        reply_error(out, d, JNULL, -32600, "Invalid request");
        return;
    }

    const char *method = jstr_get(d, msg, "method", NULL);
    jref id = jget(d, msg, "id");
    bool has_id = jhas(d, msg, "id");
    jref params = jget(d, msg, "params");
    if (!jis_obj(d, params))
        params = JNULL;

    if (!has_id) {
        /* A notification: notifications/initialized, notifications/cancelled,
         * notifications/roots/list_changed and anything else a client sends
         * cost no line. A reply to a notification is a protocol error. */
        return;
    }
    if (!id_is_valid(d, id)) {
        reply_error(out, d, JNULL, -32600, "Invalid request: id must be a string or a number");
        return;
    }
    if (!method) {
        /* A response to a request this server never sends. */
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(out, d, id, params);
    } else if (strcmp(method, "ping") == 0) {
        reply_result(out, d, id, jobj(d));
    } else if (strcmp(method, "tools/list") == 0) {
        put_envelope(out, d, id);
        sb_put(out, ",\"result\":");
        tools_list(out);
        sb_putc(out, '}');
        write_out(out);
    } else if (strcmp(method, "tools/call") == 0) {
        if (params == JNULL)
            reply_error(out, d, id, -32602, "Missing params");
        else
            handle_call(out, d, id, params);
    } else if (strcmp(method, "resources/list") == 0 ||
               strcmp(method, "prompts/list") == 0) {
        jref r = jobj(d);
        jobj_set(d, r, strcmp(method, "prompts/list") == 0 ? "prompts" : "resources", jarr(d));
        reply_result(out, d, id, r);
    } else if (strcmp(method, "resources/templates/list") == 0) {
        jref r = jobj(d);
        jobj_set(d, r, "resourceTemplates", jarr(d));
        reply_result(out, d, id, r);
    } else if (strcmp(method, "logging/setLevel") == 0) {
        reply_result(out, d, id, jobj(d));
    } else if (strcmp(method, "completion/complete") == 0) {
        jref r = jobj(d);
        jref c = jobj(d);
        jobj_set(d, c, "values", jarr(d));
        jobj_set(d, r, "completion", c);
        reply_result(out, d, id, r);
    } else {
        reply_error(out, d, id, -32601, "Method not found");
    }
}

static void handle_message(jdoc *d, sbuf *out, const char *line, size_t len)
{
    jref msg = JNULL;
    const char *err = NULL;
    if (!json_parse(d, line, len, &msg, &err)) {
        reply_error(out, d, JNULL, -32700, err ? err : "Parse error");
        return;
    }

    if (jis_arr(d, msg)) {
        /* A batch: the transport of the 2025-03-26 revision allows it, and a
         * client that still sends one gets each answer on its own line. */
        if (jlen(d, msg) == 0) {
            reply_error(out, d, JNULL, -32600, "Invalid request: an empty batch");
            return;
        }
        for (size_t i = 0; i < jlen(d, msg); i++)
            dispatch_message(d, out, jidx(d, msg, i));
        return;
    }
    dispatch_message(d, out, msg);
}

/* ---------------------------------------------------------------- pool */

typedef struct {
    char *line;
    size_t len, cap;
    bool busy;
} slot;

static slot g_slots[DCM_MAX_INFLIGHT];
static uint32_t g_queue[DCM_MAX_INFLIGHT];
static size_t g_head, g_tail, g_count;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_space = PTHREAD_COND_INITIALIZER;
static bool g_stop;

static void *worker_main(void *arg)
{
    (void)arg;
    jdoc *d = jdoc_new();
    sbuf out;
    sb_init(&out);

    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_count == 0 && !g_stop)
            pthread_cond_wait(&g_work, &g_mu);
        if (g_count == 0) {
            pthread_mutex_unlock(&g_mu);
            break;
        }
        uint32_t i = g_queue[g_head];
        g_head = (g_head + 1) % DCM_MAX_INFLIGHT;
        g_count--;
        pthread_mutex_unlock(&g_mu);

        handle_message(d, &out, g_slots[i].line, g_slots[i].len);
        /* A method with no reply leaves its text in out for the next call. */
        sb_reset(&out);

        pthread_mutex_lock(&g_mu);
        g_slots[i].busy = false;
        pthread_cond_signal(&g_space);
        pthread_mutex_unlock(&g_mu);
    }

    jdoc_free(d);
    sb_free(&out);
    return NULL;
}

/* Waits for a free slot, then copies line into it and wakes a worker. */
static void submit(const char *line, size_t len)
{
    pthread_mutex_lock(&g_mu);
    size_t i = DCM_MAX_INFLIGHT;
    for (;;) {
        for (size_t k = 0; k < DCM_MAX_INFLIGHT; k++) {
            if (!g_slots[k].busy) {
                i = k;
                break;
            }
        }
        if (i < DCM_MAX_INFLIGHT)
            break;
        pthread_cond_wait(&g_space, &g_mu);
    }
    slot *s = &g_slots[i];
    if (len + 1 > s->cap) {
        s->cap = len + 1;
        s->line = xrealloc(s->line, s->cap);
    }
    memcpy(s->line, line, len);
    s->line[len] = '\0';
    s->len = len;
    s->busy = true;
    g_queue[g_tail] = (uint32_t)i;
    g_tail = (g_tail + 1) % DCM_MAX_INFLIGHT;
    g_count++;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_mu);
}

static void start_workers(void)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DCM_WORKER_STACK);
    for (int i = 0; i < DCM_WORKERS; i++) {
        pthread_t t;
        if (pthread_create(&t, &attr, worker_main, NULL) != 0) {
            fputs("dcm: cannot start a worker\n", stderr);
            exit(2);
        }
        pthread_detach(t);
    }
    pthread_attr_destroy(&attr);
}

static void drain(void)
{
    pthread_mutex_lock(&g_mu);
    for (;;) {
        bool busy = false;
        for (size_t k = 0; k < DCM_MAX_INFLIGHT; k++)
            busy = busy || g_slots[k].busy;
        if (!busy)
            break;
        pthread_cond_wait(&g_space, &g_mu);
    }
    g_stop = true;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_mu);
}

/* ---------------------------------------------------------------- input */

/* Reads one line into *buf, without the newline. It never keeps more than
 * DCM_MAX_REQUEST_BYTES bytes: a longer line is drained as it arrives, so a
 * client that sends a gigabyte costs no gigabyte. Returns the length, -1 for a
 * line that was too long, or 0 at end of input. */
static ssize_t read_request(FILE *f, char **buf, size_t *cap)
{
    size_t n = 0;
    bool over = false;
    if (*cap == 0) {
        *cap = 64 * 1024;
        *buf = xmalloc(*cap);
    }
    for (;;) {
        char *dst;
        size_t room;
        if (over) {
            char scratch[8192];
            if (!fgets(scratch, sizeof scratch, f))
                break;
            if (strchr(scratch, '\n'))
                break;
            continue;
        }
        if (n + 1 >= *cap) {
            if (*cap >= DCM_MAX_REQUEST_BYTES) {
                over = true;
                continue;
            }
            *cap *= 2;
            *buf = xrealloc(*buf, *cap);
        }
        dst = *buf + n;
        room = *cap - n;
        if (!fgets(dst, (int)room, f)) {
            if (n == 0 && !over)
                return 0;
            break;
        }
        size_t got = strlen(dst);
        n += got;
        if (got && dst[got - 1] == '\n')
            break;
    }
    if (over)
        return -1;
    while (n && ((*buf)[n - 1] == '\n' || (*buf)[n - 1] == '\r'))
        (*buf)[--n] = '\0';
    return (ssize_t)n;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("desktop-commander %s (C11)\n", DCM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--version] [--help]\n\n"
                   "An MCP server over stdio: files, search and terminal sessions.\n"
                   "Configuration: src/config.h, set at build time.\n",
                   argv[0]);
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    start_workers();

    char *line = NULL;
    size_t cap = 0;
    jdoc *rd = jdoc_new();
    for (;;) {
        ssize_t n = read_request(stdin, &line, &cap);
        if (n == 0)
            break;
        if (n < 0) {
            /* The line was drained without being kept: a client that sends a
             * request larger than the cap gets one error and the loop goes on. */
            sbuf e;
            sb_init(&e);
            reply_error(&e, rd, JNULL, -32600, "Request is larger than DCM_MAX_REQUEST_BYTES");
            sb_free(&e);
            continue;
        }
        submit(line, (size_t)n);
    }
    free(line);
    jdoc_free(rd);
    drain();
    history_shutdown();
    return 0;
}
