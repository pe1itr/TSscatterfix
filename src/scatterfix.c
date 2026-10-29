#include "scatterfix.h"
#include "continuity.h"
#include "contest.h"
#include "ml_model.h"
#include "repair.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t pts_valid, pcr_valid;
    uint64_t next_pts, next_pcr;
} TimestampRepairState;

struct Scatterfix {
    ScatterfixConfig config;
    TsOutput output;
    Stats stats;
    PsiContext psi;
    ContinuityContext continuity;
    MlModel ml;
    RepairContext repair;
    ContestContext contest;
    uint8_t clean_cc_valid[TS_PID_COUNT], clean_next_cc[TS_PID_COUNT];
    uint8_t clean_video_cc_valid[TS_PID_COUNT], clean_video_next_cc[TS_PID_COUNT];
    TimestampRepairState clean_ts_time;
    uint8_t pending[TS_PACKET_SIZE];
    size_t pending_size;
    int failed, skipping;
};

static void make_null_packet(uint8_t raw[TS_PACKET_SIZE]) {
    raw[0] = TS_SYNC_BYTE;
    raw[1] = 0x1f;
    raw[2] = 0xff;
    raw[3] = 0x10;
    for (size_t i = 4; i < TS_PACKET_SIZE; i++) {
        raw[i] = 0xff;
    }
}

static int pid_in_list(uint16_t pid, const uint16_t *pids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pids[i] == pid) {
            return 1;
        }
    }
    return 0;
}

static int clean_ts_pid_allowed(const PsiContext *psi, uint16_t pid, int drop_audio) {
    if (pid == TS_NULL_PID || pid == 0x0000) {
        return 1;
    }
    if (!psi->has_pmt) {
        return 1;
    }
    if (pid == psi->primary_pmt_pid || pid == psi->pcr_pid) {
        return 1;
    }
    if (pid_in_list(pid, psi->video_pids, psi->video_count)) {
        return 1;
    }
    if (!drop_audio && pid_in_list(pid, psi->audio_pids, psi->audio_count)) {
        return 1;
    }
    return 0;
}

static int clean_ts_replace_bad_psi(const PsiContext *psi, TsPacket *pkt, uint8_t raw[TS_PACKET_SIZE],
                                    Stats *stats, int verbose) {
    const uint8_t *cached = NULL;
    const char *kind = NULL;
    uint8_t cc = pkt->continuity_counter;

    if (!psi_pid_is_known_si(psi, pkt->pid) || psi_packet_has_valid_section(psi, pkt)) {
        return 0;
    }

    if (pkt->pid == 0x0000 && psi->has_cached_pat) {
        cached = psi->cached_pat;
        kind = "PAT";
    } else if (psi_pid_is_pmt(psi, pkt->pid) && psi->has_cached_pmt) {
        cached = psi->cached_pmt;
        kind = "PMT";
    } else if (pkt->pid == 0x0011 && psi->has_cached_sdt) {
        cached = psi->cached_sdt;
        kind = "SDT";
    }

    if (cached) {
        memcpy(raw, cached, TS_PACKET_SIZE);
        raw[3] = (uint8_t)((raw[3] & 0xf0) | (cc & 0x0f));
        stats->clean_ts_psi_cache_repairs++;
        stats->repair_actions++;
        if (verbose) {
            fprintf(stderr,
                    "[repair] action=replace_bad_psi_from_cache pid=0x%04x table=%s reason=bad_or_unparseable_section confidence=0.80\n",
                    pkt->pid, kind);
        }
        return 1;
    }

    make_null_packet(raw);
    stats->clean_ts_null_packets++;
    stats->clean_ts_bad_psi_nulls++;
    stats->repair_actions++;
    if (verbose) {
        fprintf(stderr,
                "[repair] action=replace_with_null pid=0x%04x reason=bad_psi_without_cache confidence=0.70\n",
                pkt->pid);
    }
    return 1;
}

static void clean_ts_rewrite_psi_cc(const PsiContext *psi, const TsPacket *pkt, uint8_t raw[TS_PACKET_SIZE],
                                    uint8_t cc_valid[TS_PID_COUNT], uint8_t next_cc[TS_PID_COUNT],
                                    int verbose) {
    if (!psi_pid_is_known_si(psi, pkt->pid) || !pkt->has_payload) {
        return;
    }
    if (!cc_valid[pkt->pid]) {
        cc_valid[pkt->pid] = 1;
        next_cc[pkt->pid] = 0;
    }
    uint8_t old_cc = (uint8_t)(raw[3] & 0x0f);
    uint8_t cc = next_cc[pkt->pid];
    raw[3] = (uint8_t)((raw[3] & 0xf0) | (cc & 0x0f));
    next_cc[pkt->pid] = (uint8_t)((cc + 1u) & 0x0f);
    if (verbose && old_cc != cc) {
        fprintf(stderr,
                "[repair] action=rewrite_psi_continuity pid=0x%04x old=%u new=%u reason=clean_ts_regenerated_psi confidence=0.85\n",
                pkt->pid, old_cc, cc);
    }
}

static void clean_ts_rewrite_video_cc(const PsiContext *psi, const TsPacket *pkt, uint8_t raw[TS_PACKET_SIZE],
                                      uint8_t cc_valid[TS_PID_COUNT], uint8_t next_cc[TS_PID_COUNT],
                                      Stats *stats, int verbose) {
    if (!pkt->has_payload || !pid_in_list(pkt->pid, psi->video_pids, psi->video_count)) {
        return;
    }
    if (!cc_valid[pkt->pid]) {
        cc_valid[pkt->pid] = 1;
        next_cc[pkt->pid] = pkt->continuity_counter;
    }
    uint8_t old_cc = (uint8_t)(raw[3] & 0x0f);
    uint8_t cc = next_cc[pkt->pid];
    raw[3] = (uint8_t)((raw[3] & 0xf0) | (cc & 0x0f));
    next_cc[pkt->pid] = (uint8_t)((cc + 1u) & 0x0f);
    if (old_cc != cc) {
        stats->clean_ts_media_cc_rewrites++;
        stats->repair_actions++;
        if (verbose) {
            fprintf(stderr,
                    "[repair] action=rewrite_video_continuity pid=0x%04x old=%u new=%u reason=smooth_video_cc confidence=0.55\n",
                    pkt->pid, old_cc, cc);
        }
    }
}

static uint64_t read_pts33(const uint8_t *p) {
    return (((uint64_t)((p[0] >> 1) & 0x07)) << 30) |
           ((uint64_t)p[1] << 22) |
           (((uint64_t)((p[2] >> 1) & 0x7f)) << 15) |
           ((uint64_t)p[3] << 7) |
           ((uint64_t)((p[4] >> 1) & 0x7f));
}

static void write_pts33(uint8_t *p, uint64_t value) {
    value &= 0x1ffffffffULL;
    p[0] = (uint8_t)((p[0] & 0xf0) | (((value >> 30) & 0x07) << 1) | 1);
    p[1] = (uint8_t)(value >> 22);
    p[2] = (uint8_t)((((value >> 15) & 0x7f) << 1) | 1);
    p[3] = (uint8_t)(value >> 7);
    p[4] = (uint8_t)(((value & 0x7f) << 1) | 1);
}

static int64_t pts_delta(uint64_t newer, uint64_t older) {
    int64_t d = (int64_t)((newer - older) & 0x1ffffffffULL);
    if (d > (int64_t)0x100000000ULL) {
        d -= (int64_t)0x200000000ULL;
    }
    return d;
}

static void write_pcr_base(uint8_t *p, uint64_t base) {
    base &= 0x1ffffffffULL;
    p[0] = (uint8_t)(base >> 25);
    p[1] = (uint8_t)(base >> 17);
    p[2] = (uint8_t)(base >> 9);
    p[3] = (uint8_t)(base >> 1);
    p[4] = (uint8_t)(((base & 1u) << 7) | 0x7e);
    p[5] = 0;
}

static void clean_ts_smooth_timestamps(const PsiContext *psi, const TsPacket *pkt, uint8_t raw[TS_PACKET_SIZE],
                                       TimestampRepairState *state, Stats *stats, int verbose) {
    const uint64_t frame_ticks = 3600;
    const uint64_t pcr_ticks = 3600;
    const int64_t max_jump = 90000;

    if (!pid_in_list(pkt->pid, psi->video_pids, psi->video_count)) {
        return;
    }

    if (pkt->has_pcr && raw[3] >> 4 != 1 && raw[4] >= 7) {
        uint64_t out = pkt->pcr_base;
        int rewrite = 0;
        if (!state->pcr_valid) {
            state->pcr_valid = 1;
            state->next_pcr = pkt->pcr_base + pcr_ticks;
        } else {
            int64_t d = pts_delta(pkt->pcr_base, state->next_pcr);
            if (d < 0 || d > max_jump) {
                out = state->next_pcr;
                rewrite = 1;
            }
            state->next_pcr = out + pcr_ticks;
        }
        if (rewrite) {
            write_pcr_base(&raw[6], out);
            stats->clean_ts_pcr_rewrites++;
            stats->repair_actions++;
            if (verbose) {
                fprintf(stderr,
                        "[repair] action=rewrite_pcr pid=0x%04x old=%llu new=%llu reason=smooth_timestamp_outlier confidence=0.50\n",
                        pkt->pid, (unsigned long long)pkt->pcr_base, (unsigned long long)out);
            }
        }
    }

    if (!pkt->payload_unit_start || !pkt->has_payload || pkt->payload_size < 14) {
        return;
    }
    size_t off = pkt->payload_offset;
    if (off + 14 > TS_PACKET_SIZE || raw[off] != 0x00 || raw[off + 1] != 0x00 || raw[off + 2] != 0x01) {
        return;
    }
    uint8_t flags = (uint8_t)((raw[off + 7] >> 6) & 0x03);
    uint8_t header_len = raw[off + 8];
    if (!(flags & 0x02) || off + 9u + header_len > TS_PACKET_SIZE || off + 14 > TS_PACKET_SIZE) {
        return;
    }

    uint64_t old_pts = read_pts33(&raw[off + 9]);
    uint64_t new_pts = old_pts;
    int rewrite_pts = 0;
    if (!state->pts_valid) {
        state->pts_valid = 1;
        state->next_pts = old_pts + frame_ticks;
    } else {
        int64_t d = pts_delta(old_pts, state->next_pts);
        if (d < 0 || d > max_jump) {
            new_pts = state->next_pts;
            rewrite_pts = 1;
        }
        state->next_pts = new_pts + frame_ticks;
    }
    if (rewrite_pts) {
        write_pts33(&raw[off + 9], new_pts);
        stats->clean_ts_pts_rewrites++;
        stats->repair_actions++;
        if (verbose) {
            fprintf(stderr,
                    "[repair] action=rewrite_pts pid=0x%04x old=%llu new=%llu reason=smooth_timestamp_outlier confidence=0.50\n",
                    pkt->pid, (unsigned long long)old_pts, (unsigned long long)new_pts);
        }
    }
    if ((flags & 0x01) && off + 19 <= TS_PACKET_SIZE) {
        uint64_t old_dts = read_pts33(&raw[off + 14]);
        uint64_t new_dts = new_pts;
        if (rewrite_pts || pts_delta(old_dts, new_pts) < -max_jump || pts_delta(old_dts, new_pts) > max_jump) {
            write_pts33(&raw[off + 14], new_dts);
        }
    }
}


void scatterfix_config_init(ScatterfixConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->psi_interval_ms = 500;
    config->ml_enabled = 1;
}

Scatterfix *scatterfix_create(const ScatterfixConfig *config,
                             ScatterfixWrite write, void *userdata) {
    Scatterfix *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    scatterfix_config_init(&ctx->config);
    if (config) ctx->config = *config;
    if (!write && !ctx->config.dry_run) { free(ctx); return NULL; }
    ts_output_init_callback(&ctx->output, write, userdata);
    scatterfix_reset(ctx);
    return ctx;
}

void scatterfix_reset(Scatterfix *ctx) {
    if (!ctx) return;
    psi_init(&ctx->psi);
    continuity_init(&ctx->continuity);
    contest_init(&ctx->contest);
    ml_model_init(&ctx->ml, ctx->config.ml_enabled, ctx->config.contest_mode, NULL);
    repair_init(&ctx->repair, ctx->config.psi_interval_ms, ctx->config.verbose, ctx->config.dry_run);
    memset(ctx->clean_cc_valid, 0, sizeof(ctx->clean_cc_valid));
    memset(ctx->clean_next_cc, 0, sizeof(ctx->clean_next_cc));
    memset(ctx->clean_video_cc_valid, 0, sizeof(ctx->clean_video_cc_valid));
    memset(ctx->clean_video_next_cc, 0, sizeof(ctx->clean_video_next_cc));
    memset(&ctx->clean_ts_time, 0, sizeof(ctx->clean_ts_time));
    ctx->pending_size = 0;
    ctx->failed = ctx->skipping = 0;
}

static int process_packet(Scatterfix *ctx, uint8_t raw[TS_PACKET_SIZE]) {
    ctx->stats.packets_read++;
        TsPacket pkt;
        if (!ts_packet_parse(raw, &pkt)) {
            ctx->stats.invalid_packets++;
            if ((ctx->config.sanitize || ctx->config.clean_ts) && !ctx->config.passthrough) {
                make_null_packet(raw);
                ctx->stats.repair_actions++;
                if (ctx->config.clean_ts) {
                    ctx->stats.clean_ts_null_packets++;
                }
                if (ctx->config.verbose) {
                    fprintf(stderr, "[repair] action=replace_with_null reason=invalid_ts_header confidence=0.85\n");
                }
            }
            if (ctx->config.verbose) {
                fprintf(stderr, "[repair] action=%s reason=invalid_ts_header confidence=0.65\n",
                        ((ctx->config.sanitize || ctx->config.clean_ts) && !ctx->config.passthrough) ? "replace_with_null" : "pass");
            }
            if (!repair_write_packet(&ctx->repair, &ctx->output, raw, &ctx->stats)) {
                if (ctx->config.verbose) fprintf(stderr, "scatterfix: write failed\n");
                return 0;
            }
            return 1;
        }

        if (!ctx->config.passthrough && pkt.transport_error) {
            ctx->stats.transport_error_packets++;
            if (repair_replace_tei_from_cache(&ctx->repair, &ctx->psi, &pkt, raw, &ctx->stats)) {
                if (!ts_packet_parse(raw, &pkt)) {
                    ctx->stats.packets_dropped++;
                    ctx->stats.invalid_packets++;
                    return 1;
                }
            } else if (ctx->config.sanitize || ctx->config.clean_ts) {
                make_null_packet(raw);
                ctx->stats.repair_actions++;
                if (ctx->config.clean_ts) {
                    ctx->stats.clean_ts_null_packets++;
                }
                if (ctx->config.verbose) {
                    fprintf(stderr, "[repair] action=replace_with_null pid=0x%04x reason=unrepairable_tei confidence=0.80\n",
                            pkt.pid);
                }
                if (!repair_write_packet(&ctx->repair, &ctx->output, raw, &ctx->stats)) {
                    if (ctx->config.verbose) fprintf(stderr, "scatterfix: write failed\n");
                    return 0;
                }
                return 1;
            }
        }

        if (ctx->config.clean_ts && !ctx->config.passthrough && clean_ts_replace_bad_psi(&ctx->psi, &pkt, raw, &ctx->stats, ctx->config.verbose)) {
            if (!ts_packet_parse(raw, &pkt)) {
                ctx->stats.packets_dropped++;
                ctx->stats.invalid_packets++;
                return 1;
            }
        }

        continuity_observe(&ctx->continuity, &pkt, &ctx->stats, ctx->config.verbose);
        psi_observe_packet(&ctx->psi, &pkt, ctx->stats.packets_read, ctx->config.verbose);
        if (ctx->config.contest_mode) {
            contest_observe_packet(&ctx->contest, &pkt, &ctx->psi, ctx->stats.packets_read);
        }
        ml_model_observe(&ctx->ml, &pkt, &ctx->psi, &ctx->stats, ctx->stats.packets_read, ctx->config.verbose);

        if (ctx->config.clean_ts && !ctx->config.passthrough && !clean_ts_pid_allowed(&ctx->psi, pkt.pid, ctx->config.drop_audio)) {
            uint16_t filtered_pid = pkt.pid;
            make_null_packet(raw);
            ctx->stats.clean_ts_null_packets++;
            ctx->stats.clean_ts_filtered_pids++;
            ctx->stats.repair_actions++;
            if (ctx->config.verbose) {
                fprintf(stderr,
                        "[repair] action=replace_with_null pid=0x%04x reason=clean_ts_unknown_pid_after_pmt confidence=0.75\n",
                        filtered_pid);
            }
        }

        if (ctx->config.clean_ts && !ctx->config.passthrough) {
            TsPacket out_pkt;
            if (ts_packet_parse(raw, &out_pkt)) {
                clean_ts_rewrite_psi_cc(&ctx->psi, &out_pkt, raw, ctx->clean_cc_valid, ctx->clean_next_cc, ctx->config.verbose);
                if (ctx->config.smooth_video_cc) {
                    clean_ts_rewrite_video_cc(&ctx->psi, &out_pkt, raw, ctx->clean_video_cc_valid,
                                              ctx->clean_video_next_cc, &ctx->stats, ctx->config.verbose);
                }
                if (ctx->config.smooth_timestamps) {
                    clean_ts_smooth_timestamps(&ctx->psi, &out_pkt, raw, &ctx->clean_ts_time, &ctx->stats, ctx->config.verbose);
                }
            }
        }

        if (!ctx->config.passthrough && !repair_maybe_inject_psi(&ctx->repair, &ctx->output, &ctx->psi, &ctx->stats)) {
            if (ctx->config.verbose) fprintf(stderr, "scatterfix: write failed during PSI injection\n");
            return 0;
        }
        if (!repair_write_packet(&ctx->repair, &ctx->output, raw, &ctx->stats)) {
            if (ctx->config.verbose) fprintf(stderr, "scatterfix: write failed\n");
            return 0;
        }


    return 1;
}

int scatterfix_feed(Scatterfix *ctx, const uint8_t *data, size_t size) {
    if (!ctx || (!data && size) || ctx->failed) return 0;
    while (size) {
        if (!ctx->pending_size && *data != TS_SYNC_BYTE) {
            ctx->stats.bytes_dropped++;
            if (!ctx->skipping) ctx->stats.resync_count++;
            ctx->skipping = 1;
            data++; size--;
            continue;
        }
        ctx->skipping = 0;
        size_t n = TS_PACKET_SIZE - ctx->pending_size;
        if (n > size) n = size;
        memcpy(ctx->pending + ctx->pending_size, data, n);
        ctx->pending_size += n;
        data += n; size -= n;
        if (ctx->pending_size == TS_PACKET_SIZE) {
            ctx->pending_size = 0;
            if (!process_packet(ctx, ctx->pending)) { ctx->failed = 1; return 0; }
        }
    }
    return 1;
}

int scatterfix_finish(Scatterfix *ctx) {
    if (!ctx || ctx->failed) return 0;
    if (ctx->pending_size) {
        ctx->stats.bytes_dropped += ctx->pending_size;
        ctx->stats.packets_dropped++;
        ctx->pending_size = 0;
    }
    return 1;
}
void scatterfix_destroy(Scatterfix *ctx) { free(ctx); }
const Stats *scatterfix_stats(const Scatterfix *ctx) { return ctx ? &ctx->stats : NULL; }
const PsiContext *scatterfix_service(const Scatterfix *ctx) { return ctx ? &ctx->psi : NULL; }
void scatterfix_print_status(const Scatterfix *ctx, FILE *file, uint64_t index) {
    if (ctx && file) contest_print_json(file, &ctx->contest, &ctx->stats, &ctx->psi, index);
}
void scatterfix_print_roles(const Scatterfix *ctx, FILE *file) {
    if (ctx && file) ml_model_print_roles(file, &ctx->ml, &ctx->psi);
}
int scatterfix_load_state(Scatterfix *ctx, const char *path) {
    if (!ctx || !path) return 0;
    snprintf(ctx->ml.state_path, sizeof(ctx->ml.state_path), "%s", path);
    int result = ml_model_load(&ctx->ml);
    ctx->ml.state_path[0] = 0;
    return result;
}
int scatterfix_save_state(Scatterfix *ctx, const char *path) {
    if (!ctx || !path) return 0;
    snprintf(ctx->ml.state_path, sizeof(ctx->ml.state_path), "%s", path);
    int result = ml_model_save(&ctx->ml);
    ctx->ml.state_path[0] = 0;
    return result;
}
