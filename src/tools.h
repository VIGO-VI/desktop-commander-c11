/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#ifndef DCM_TOOLS_H
#define DCM_TOOLS_H

#include "json.h"

#include <stdbool.h>

/* A tool result lives in the request's document, so nothing is freed here. */
typedef struct {
    jref content;    /* array of content blocks */
    jref structured; /* object, or JNULL */
    bool is_error;
} tool_result;

tool_result result_text(jdoc *d, const char *text);
tool_result result_error(jdoc *d, const char *fmt, ...);
tool_result result_text_struct(jdoc *d, const char *text, jref structured);

/* Each tool takes the request document and the arguments object (never
 * JNULL) and returns a result in the same document. */
tool_result tool_get_config(jdoc *d, jref a);
tool_result tool_set_config_value(jdoc *d, jref a);
tool_result tool_read_file(jdoc *d, jref a);
tool_result tool_read_multiple_files(jdoc *d, jref a);
tool_result tool_write_file(jdoc *d, jref a);
tool_result tool_create_directory(jdoc *d, jref a);
tool_result tool_list_directory(jdoc *d, jref a);
tool_result tool_move_file(jdoc *d, jref a);
tool_result tool_get_file_info(jdoc *d, jref a);
tool_result tool_edit_block(jdoc *d, jref a);

tool_result tool_start_search(jdoc *d, jref a);
tool_result tool_get_more_search_results(jdoc *d, jref a);
tool_result tool_stop_search(jdoc *d, jref a);
tool_result tool_list_searches(jdoc *d, jref a);

tool_result tool_start_process(jdoc *d, jref a);
tool_result tool_read_process_output(jdoc *d, jref a);
tool_result tool_interact_with_process(jdoc *d, jref a);
tool_result tool_force_terminate(jdoc *d, jref a);
tool_result tool_list_sessions(jdoc *d, jref a);
tool_result tool_list_processes(jdoc *d, jref a);
tool_result tool_kill_process(jdoc *d, jref a);

tool_result tool_get_usage_stats(jdoc *d, jref a);
tool_result tool_get_recent_tool_calls(jdoc *d, jref a);

/* Shared: record a call for get_usage_stats / get_recent_tool_calls. */
void history_record(jdoc *d, const char *tool, jref args, const tool_result *r);
void history_shutdown(void);

#endif
