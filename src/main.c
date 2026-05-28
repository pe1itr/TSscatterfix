#include "continuity.h"
#include "contest.h"
#include "io.h"
#include "ml_model.h"
#include "psi.h"
#include "repair.h"
#include "stats.h"
#include "ts_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DEFAULT_FILE_REPLAY_UDP_RATE_KBIT 350

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *udp_input_port;
    const char *udp_output_host_port;
    const char *ml_state_path;
    const char *json_status_path;
    unsigned psi_interval_ms;
    unsigned stats_interval;
    unsigned max_packets;
    unsigned udp_rate_kbit;
    int psi_interval_set;
    int loop_input;
    int passthrough;
    int sanitize;
    int clean_ts;
    int drop_audio;
    int smooth_video_cc;
    int smooth_timestamps;
    int verbose;
    int dry_run;
    int ml_enabled;
    int contest_mode;
    int json_status;
    int normal_mode_seen;
    int contest_mode_seen;
} Options;

typedef struct {
    uint8_t pts_valid;
    uint8_t pcr_valid;
    uint64_t next_pts;
    uint64_t next_pcr;
} TimestampRepairState;

static void usage(FILE *f) {
    fprintf(f,
            "Usage: tsscatterfix [options]\n"
            "\n"
            "Options:\n"
            "  --input PATH            Read TS from PATH instead of stdin\n"
            "  --output PATH           Write TS to PATH instead of stdout\n"
            "  --udp-input PORT        Read MPEG-TS over UDP by binding PORT\n"
            "  --udp-output HOST:PORT  Write MPEG-TS packets to UDP destination\n"
            "  --udp-rate-kbit N       Pace UDP output for file/stdin replay\n"
            "  --loop-input            Loop regular file input, useful for UDP replay tests\n"
            "  --psi-interval-ms N     Periodically inject cached PAT/PMT/SDT, default 500\n"
            "  --no-psi-inject         Disable periodic cached PAT/PMT/SDT injection\n"
            "  --passthrough           Do not modify packets; useful for I/O testing\n"
            "  --sanitize              Replace unrepairable corrupt/TEI packets with null packets\n"
            "  --clean-ts              Preserve known PAT/PMT/video/audio/PCR and null other PIDs after lock\n"
            "  --drop-audio            In clean TS mode, replace audio PID packets with null packets\n"
            "  --smooth-video-cc       In clean TS mode, rewrite video continuity counters monotonically\n"
            "  --smooth-timestamps     In clean TS mode, repair large video PTS/PCR outliers\n"
            "  --stats-interval N      Print stats every N input packets, default 10000\n"
            "  --max-packets N         Stop after N input packets, useful for tests/captures\n"
            "  --mode normal|contest   Select normal synchronous repair or contest recovered-view mode\n"
            "  --normal-mode           Explicitly select normal synchronous repair mode, default\n"
            "  --contest-mode          Async recovered-view mode for static DATV test images\n"
            "  --json-status           Print machine-readable JSON status lines to stderr\n"
            "  --json-status-output P  Write JSON status lines to file P instead of stderr\n"
            "  --verbose               Log repair decisions and parser events to stderr\n"
            "  --dry-run               Analyze but do not write TS output\n"
            "  --no-ml                 Disable probabilistic role inference\n"
            "  --ml-state PATH         Load/save minimal ML state\n"
            "  --help                  Show this help\n");
}

static int parse_uint(const char *s, unsigned *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s[0] || *end != '\0' || v > 0xfffffffful) {
        return 0;
    }
    *out = (unsigned)v;
    return 1;
}

static int select_mode(Options *opt, int contest) {
    if (contest) {
        if (opt->normal_mode_seen) {
            return 0;
        }
        opt->contest_mode = 1;
        opt->contest_mode_seen = 1;
    } else {
        if (opt->contest_mode_seen) {
            return 0;
        }
        opt->contest_mode = 0;
        opt->normal_mode_seen = 1;
    }
    return 1;
}

static int parse_options(int argc, char **argv, Options *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->psi_interval_ms = 500;
    opt->stats_interval = 10000;
    opt->ml_enabled = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            opt->input_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            opt->output_path = argv[++i];
        } else if (strcmp(argv[i], "--udp-input") == 0 && i + 1 < argc) {
            opt->udp_input_port = argv[++i];
        } else if (strcmp(argv[i], "--udp-output") == 0 && i + 1 < argc) {
            opt->udp_output_host_port = argv[++i];
        } else if (strcmp(argv[i], "--udp-rate-kbit") == 0 && i + 1 < argc) {
            if (!parse_uint(argv[++i], &opt->udp_rate_kbit)) return 0;
        } else if (strcmp(argv[i], "--loop-input") == 0) {
            opt->loop_input = 1;
        } else if (strcmp(argv[i], "--psi-interval-ms") == 0 && i + 1 < argc) {
            if (!parse_uint(argv[++i], &opt->psi_interval_ms)) return 0;
            opt->psi_interval_set = 1;
        } else if (strcmp(argv[i], "--no-psi-inject") == 0) {
            opt->psi_interval_ms = 0;
            opt->psi_interval_set = 1;
        } else if (strcmp(argv[i], "--passthrough") == 0 || strcmp(argv[i], "--no-repair") == 0) {
            opt->passthrough = 1;
            opt->psi_interval_ms = 0;
            opt->psi_interval_set = 1;
        } else if (strcmp(argv[i], "--sanitize") == 0) {
            opt->sanitize = 1;
        } else if (strcmp(argv[i], "--clean-ts") == 0) {
            opt->clean_ts = 1;
            opt->sanitize = 1;
        } else if (strcmp(argv[i], "--drop-audio") == 0) {
            opt->drop_audio = 1;
        } else if (strcmp(argv[i], "--smooth-video-cc") == 0) {
            opt->smooth_video_cc = 1;
        } else if (strcmp(argv[i], "--smooth-timestamps") == 0) {
            opt->smooth_timestamps = 1;
        } else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc) {
            if (!parse_uint(argv[++i], &opt->stats_interval)) return 0;
        } else if (strcmp(argv[i], "--max-packets") == 0 && i + 1 < argc) {
            if (!parse_uint(argv[++i], &opt->max_packets)) return 0;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "normal") == 0) {
                if (!select_mode(opt, 0)) return 0;
            } else if (strcmp(mode, "contest") == 0) {
                if (!select_mode(opt, 1)) return 0;
            } else {
                return 0;
            }
        } else if (strcmp(argv[i], "--normal-mode") == 0) {
            if (!select_mode(opt, 0)) return 0;
        } else if (strcmp(argv[i], "--contest-mode") == 0) {
            if (!select_mode(opt, 1)) return 0;
        } else if (strcmp(argv[i], "--json-status") == 0) {
            opt->json_status = 1;
        } else if (strcmp(argv[i], "--json-status-output") == 0 && i + 1 < argc) {
            opt->json_status = 1;
            opt->json_status_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opt->verbose = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            opt->dry_run = 1;
        } else if (strcmp(argv[i], "--no-ml") == 0) {
            opt->ml_enabled = 0;
        } else if (strcmp(argv[i], "--ml-state") == 0 && i + 1 < argc) {
            opt->ml_state_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            exit(0);
        } else {
            return 0;
        }
    }
    if ((opt->input_path && opt->udp_input_port) || (opt->output_path && opt->udp_output_host_port)) {
        return 0;
    }
    return 1;
}

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

static int stdin_is_regular(void) {
#ifdef _WIN32
    struct _stat st;
    if (_fstat(_fileno(stdin), &st) != 0) {
        return 0;
    }
    return (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    if (fstat(STDIN_FILENO, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
#endif
}

int main(int argc, char **argv) {
    Options opt;
    if (!parse_options(argc, argv, &opt)) {
        usage(stderr);
        return 2;
    }

    if (!ts_io_startup()) {
        fprintf(stderr, "tsscatterfix: socket startup failed\n");
        return 1;
    }

    FILE *in_file = stdin;
    FILE *out_file = stdout;
    FILE *json_status_file = stderr;
    TsInput input;
    TsOutput output;
    ts_input_init_file(&input, stdin);
    ts_output_init_file(&output, stdout);

    if (opt.input_path) {
        in_file = fopen(opt.input_path, "rb");
        if (!in_file) {
            fprintf(stderr, "tsscatterfix: cannot open input '%s': %s\n", opt.input_path, strerror(errno));
            ts_io_cleanup();
            return 1;
        }
        ts_input_init_file(&input, in_file);
    } else if (opt.udp_input_port) {
        if (!ts_input_init_udp(&input, opt.udp_input_port)) {
            fprintf(stderr, "tsscatterfix: cannot bind UDP input port '%s'\n", opt.udp_input_port);
            ts_io_cleanup();
            return 1;
        }
        if (opt.verbose) {
            fprintf(stderr, "[io] udp_input port=%s\n", opt.udp_input_port);
        }
    }
    if (opt.output_path && !opt.dry_run) {
        out_file = fopen(opt.output_path, "wb");
        if (!out_file) {
            fprintf(stderr, "tsscatterfix: cannot open output '%s': %s\n", opt.output_path, strerror(errno));
            ts_input_close(&input);
            if (in_file != stdin) fclose(in_file);
            ts_io_cleanup();
            return 1;
        }
        ts_output_init_file(&output, out_file);
    } else if (opt.udp_output_host_port && !opt.dry_run) {
        if (!ts_output_init_udp(&output, opt.udp_output_host_port)) {
            fprintf(stderr, "tsscatterfix: cannot open UDP output '%s'\n", opt.udp_output_host_port);
            ts_input_close(&input);
            if (in_file != stdin) fclose(in_file);
            ts_io_cleanup();
            return 1;
        }
        if (opt.verbose) {
            fprintf(stderr, "[io] udp_output target=%s\n", opt.udp_output_host_port);
        }
    }
    if (opt.json_status_path) {
        json_status_file = fopen(opt.json_status_path, "w");
        if (!json_status_file) {
            fprintf(stderr, "tsscatterfix: cannot open JSON status output '%s': %s\n",
                    opt.json_status_path, strerror(errno));
            ts_output_close(&output);
            ts_input_close(&input);
            if (out_file != stdout) fclose(out_file);
            if (in_file != stdin) fclose(in_file);
            ts_io_cleanup();
            return 1;
        }
    }
    if (opt.udp_rate_kbit > 0) {
        ts_output_set_udp_rate(&output, opt.udp_rate_kbit);
        if (opt.verbose) {
            fprintf(stderr, "[io] udp_rate_kbit=%u\n", opt.udp_rate_kbit);
        }
    } else if (opt.udp_output_host_port && !opt.udp_input_port && (opt.input_path || stdin_is_regular())) {
        ts_output_set_udp_rate(&output, DEFAULT_FILE_REPLAY_UDP_RATE_KBIT);
        fprintf(stderr, "[io] udp_rate_kbit=%u reason=auto_file_replay\n", DEFAULT_FILE_REPLAY_UDP_RATE_KBIT);
    }
    if (opt.udp_output_host_port && !opt.udp_input_port && (opt.input_path || stdin_is_regular()) && !opt.psi_interval_set) {
        opt.psi_interval_ms = 0;
        fprintf(stderr, "[repair] psi_inject=disabled reason=auto_file_replay_passthrough\n");
    }
    if (opt.clean_ts && !opt.psi_interval_set) {
        opt.psi_interval_ms = 0;
        fprintf(stderr, "[repair] psi_inject=disabled reason=clean_ts_cadence_preserve\n");
    }

    Stats stats;
    PsiContext psi;
    ContinuityContext continuity;
    MlModel ml;
    RepairContext repair;
    ContestContext contest;
    TsReader reader;
    uint8_t raw[TS_PACKET_SIZE];
    uint8_t clean_cc_valid[TS_PID_COUNT] = {0};
    uint8_t clean_next_cc[TS_PID_COUNT] = {0};
    uint8_t clean_video_cc_valid[TS_PID_COUNT] = {0};
    uint8_t clean_video_next_cc[TS_PID_COUNT] = {0};
    TimestampRepairState clean_ts_time = {0, 0, 0, 0};
    uint64_t stats_index = 0;

    stats_init(&stats);
    psi_init(&psi);
    continuity_init(&continuity);
    contest_init(&contest);
    ml_model_init(&ml, opt.ml_enabled, opt.contest_mode, opt.ml_state_path);
    if (opt.contest_mode && opt.verbose) {
        fprintf(stderr, "[contest] mode=enabled timing=async_recovered_view assumption=static_test_image repeat_window=%u payload_repair=planned\n",
                ml.repeat_window);
    }
    if (ml_model_load(&ml) && opt.verbose) {
        fprintf(stderr, "[ml] loaded state from %s\n", opt.ml_state_path);
    }
    repair_init(&repair, opt.psi_interval_ms, opt.verbose, opt.dry_run);
    ts_reader_init(&reader, &input, opt.verbose);

    while (1) {
        if (!ts_reader_next(&reader, raw, &stats)) {
            if (opt.loop_input && opt.input_path && !opt.udp_input_port) {
                if (fseek(in_file, 0, SEEK_SET) != 0) {
                    fprintf(stderr, "tsscatterfix: cannot loop input '%s': %s\n", opt.input_path, strerror(errno));
                    break;
                }
                ts_reader_init(&reader, &input, opt.verbose);
                if (opt.verbose) {
                    fprintf(stderr, "[io] loop_input path=%s\n", opt.input_path);
                }
                continue;
            }
            break;
        }
        TsPacket pkt;
        if (!ts_packet_parse(raw, &pkt)) {
            stats.invalid_packets++;
            if ((opt.sanitize || opt.clean_ts) && !opt.passthrough) {
                make_null_packet(raw);
                stats.repair_actions++;
                if (opt.clean_ts) {
                    stats.clean_ts_null_packets++;
                }
                if (opt.verbose) {
                    fprintf(stderr, "[repair] action=replace_with_null reason=invalid_ts_header confidence=0.85\n");
                }
            }
            if (opt.verbose) {
                fprintf(stderr, "[repair] action=%s reason=invalid_ts_header confidence=0.65\n",
                        ((opt.sanitize || opt.clean_ts) && !opt.passthrough) ? "replace_with_null" : "pass");
            }
            if (!repair_write_packet(&repair, &output, raw, &stats)) {
                fprintf(stderr, "tsscatterfix: write failed\n");
                break;
            }
            continue;
        }

        if (!opt.passthrough && pkt.transport_error) {
            stats.transport_error_packets++;
            if (repair_replace_tei_from_cache(&repair, &psi, &pkt, raw, &stats)) {
                if (!ts_packet_parse(raw, &pkt)) {
                    stats.packets_dropped++;
                    stats.invalid_packets++;
                    continue;
                }
            } else if (opt.sanitize || opt.clean_ts) {
                make_null_packet(raw);
                stats.repair_actions++;
                if (opt.clean_ts) {
                    stats.clean_ts_null_packets++;
                }
                if (opt.verbose) {
                    fprintf(stderr, "[repair] action=replace_with_null pid=0x%04x reason=unrepairable_tei confidence=0.80\n",
                            pkt.pid);
                }
                if (!repair_write_packet(&repair, &output, raw, &stats)) {
                    fprintf(stderr, "tsscatterfix: write failed\n");
                    break;
                }
                continue;
            }
        }

        if (opt.clean_ts && !opt.passthrough && clean_ts_replace_bad_psi(&psi, &pkt, raw, &stats, opt.verbose)) {
            if (!ts_packet_parse(raw, &pkt)) {
                stats.packets_dropped++;
                stats.invalid_packets++;
                continue;
            }
        }

        continuity_observe(&continuity, &pkt, &stats, opt.verbose);
        psi_observe_packet(&psi, &pkt, stats.packets_read, opt.verbose);
        if (opt.contest_mode) {
            contest_observe_packet(&contest, &pkt, &psi, stats.packets_read);
        }
        ml_model_observe(&ml, &pkt, &psi, &stats, stats.packets_read, opt.verbose);

        if (opt.clean_ts && !opt.passthrough && !clean_ts_pid_allowed(&psi, pkt.pid, opt.drop_audio)) {
            uint16_t filtered_pid = pkt.pid;
            make_null_packet(raw);
            stats.clean_ts_null_packets++;
            stats.clean_ts_filtered_pids++;
            stats.repair_actions++;
            if (opt.verbose) {
                fprintf(stderr,
                        "[repair] action=replace_with_null pid=0x%04x reason=clean_ts_unknown_pid_after_pmt confidence=0.75\n",
                        filtered_pid);
            }
        }

        if (opt.clean_ts && !opt.passthrough) {
            TsPacket out_pkt;
            if (ts_packet_parse(raw, &out_pkt)) {
                clean_ts_rewrite_psi_cc(&psi, &out_pkt, raw, clean_cc_valid, clean_next_cc, opt.verbose);
                if (opt.smooth_video_cc) {
                    clean_ts_rewrite_video_cc(&psi, &out_pkt, raw, clean_video_cc_valid,
                                              clean_video_next_cc, &stats, opt.verbose);
                }
                if (opt.smooth_timestamps) {
                    clean_ts_smooth_timestamps(&psi, &out_pkt, raw, &clean_ts_time, &stats, opt.verbose);
                }
            }
        }

        if (!opt.passthrough && !repair_maybe_inject_psi(&repair, &output, &psi, &stats)) {
            fprintf(stderr, "tsscatterfix: write failed during PSI injection\n");
            break;
        }
        if (!repair_write_packet(&repair, &output, raw, &stats)) {
            fprintf(stderr, "tsscatterfix: write failed\n");
            break;
        }

        if (opt.stats_interval > 0 && stats.packets_read % opt.stats_interval == 0) {
            stats_print(stderr, &stats, &psi, ++stats_index);
            ml_model_print_roles(stderr, &ml, &psi);
            if (opt.json_status) {
                contest_print_json(json_status_file, &contest, &stats, &psi, stats_index);
                fflush(json_status_file);
            }
        }
        if (opt.max_packets > 0 && stats.packets_read >= opt.max_packets) {
            break;
        }
    }

    if (opt.stats_interval > 0 || opt.verbose) {
        stats_print(stderr, &stats, &psi, ++stats_index);
        ml_model_print_roles(stderr, &ml, &psi);
    }
    if (opt.json_status) {
        contest_print_json(json_status_file, &contest, &stats, &psi, stats_index);
        fflush(json_status_file);
    }
    if (ml_model_save(&ml) < 0) {
        fprintf(stderr, "tsscatterfix: cannot save ML state '%s': %s\n", opt.ml_state_path, strerror(errno));
    }

    if (!ts_output_flush(&output)) {
        fprintf(stderr, "tsscatterfix: output flush failed\n");
    }
    ts_output_close(&output);
    ts_input_close(&input);
    if (json_status_file && json_status_file != stderr) fclose(json_status_file);
    if (out_file && out_file != stdout) fclose(out_file);
    if (in_file && in_file != stdin) fclose(in_file);
    ts_io_cleanup();
    return ferror(stdout) ? 1 : 0;
}
