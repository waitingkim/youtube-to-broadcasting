#pragma once

#include <util/dstr.h>
#include <stdbool.h>

/* Minimal cancellable child-process runner (POSIX only).
 *
 * Runs argv[0] (absolute path) with argv, collects stdout+stderr (merged)
 * into `output`, and returns the exit code (-1 on spawn failure / signal).
 * The running pid is published through yt_proc_cancel_all() so a shutdown
 * can terminate it. */
int yt_proc_run(const char *const *argv, struct dstr *output);

/* Same, but also calls `cb` for every complete output line as it arrives. */
typedef void (*yt_proc_line_cb)(void *opaque, const char *line);
int yt_proc_run_lines(const char *const *argv, struct dstr *output, yt_proc_line_cb cb, void *opaque);

/* Terminate every running child and make future yt_proc_run calls fail. */
void yt_proc_cancel_all(void);

/* Reset the cancelled state (module load). */
void yt_proc_init(void);
void yt_proc_free(void);
