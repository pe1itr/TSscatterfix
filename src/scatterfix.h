#ifndef SCATTERFIX_H
#define SCATTERFIX_H

#include <stddef.h>
#include <stdint.h>
#include "stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Scatterfix Scatterfix;
typedef int (*ScatterfixWrite)(void *userdata, const uint8_t *data, size_t size);
typedef struct {
    unsigned psi_interval_ms;
    int passthrough, sanitize, clean_ts, drop_audio;
    int smooth_video_cc, smooth_timestamps;
    int verbose, dry_run, ml_enabled, contest_mode;
} ScatterfixConfig;

/* Initialize before changing individual options. No global state or owned I/O.
 * Callbacks run synchronously and must return 1 on success, 0 on failure.
 * Callback data is borrowed until return. Do not reenter the same context.
 * Separate contexts can be used independently on separate threads. */
void scatterfix_config_init(ScatterfixConfig *config);
Scatterfix *scatterfix_create(const ScatterfixConfig *config,
                             ScatterfixWrite write, void *userdata);
/* Accepts arbitrary byte chunks; buffers incomplete 188-byte TS packets.
 * Failure is sticky until reset; never retry the same input after failure. */
int scatterfix_feed(Scatterfix *ctx, const uint8_t *data, size_t size);
int scatterfix_finish(Scatterfix *ctx);
/* Reset learned state and pending bytes; preserve cumulative statistics. */
void scatterfix_reset(Scatterfix *ctx);
void scatterfix_destroy(Scatterfix *ctx);
const Stats *scatterfix_stats(const Scatterfix *ctx);
const PsiContext *scatterfix_service(const Scatterfix *ctx);
void scatterfix_print_status(const Scatterfix *ctx, FILE *file, uint64_t index);
void scatterfix_print_roles(const Scatterfix *ctx, FILE *file);
int scatterfix_load_state(Scatterfix *ctx, const char *path);
int scatterfix_save_state(Scatterfix *ctx, const char *path);

#ifdef __cplusplus
}
#endif
#endif
