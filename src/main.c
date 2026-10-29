#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include "scatterfix.h"
#include "../tools/contest_cli.h"
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
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
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
    int enabled;
#ifndef _WIN32
    FILE *tty;
    int fd;
    struct termios saved_termios;
    int restore_termios;
#endif
} ResetKeyControl;

typedef struct {
    ResetKeyControl *reset_key;
    Scatterfix *engine;
} ResetControlContext;

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
            "  R key                   Reset learned stream state when run from a terminal\n"
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

static int reset_key_open(ResetKeyControl *key, int verbose) {
    memset(key, 0, sizeof(*key));
#ifdef _WIN32
    (void)verbose;
    return 0;
#else
    key->fd = -1;
    key->tty = fopen("/dev/tty", "r");
    if (!key->tty) {
        return 0;
    }
    key->fd = fileno(key->tty);
    if (key->fd < 0 || tcgetattr(key->fd, &key->saved_termios) != 0) {
        fclose(key->tty);
        key->tty = NULL;
        key->fd = -1;
        return 0;
    }

    struct termios raw = key->saved_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(key->fd, TCSANOW, &raw) != 0) {
        fclose(key->tty);
        key->tty = NULL;
        key->fd = -1;
        return 0;
    }
    key->restore_termios = 1;
    key->enabled = 1;
    if (verbose) {
        fprintf(stderr, "[control] reset_key=R source=/dev/tty\n");
    }
    return 1;
#endif
}

static void reset_key_close(ResetKeyControl *key) {
#ifdef _WIN32
    (void)key;
#else
    if (key->restore_termios && key->fd >= 0) {
        tcsetattr(key->fd, TCSANOW, &key->saved_termios);
    }
    if (key->tty) {
        fclose(key->tty);
    }
    memset(key, 0, sizeof(*key));
    key->fd = -1;
#endif
}

static int reset_key_pressed(ResetKeyControl *key) {
#ifdef _WIN32
    (void)key;
    return 0;
#else
    if (!key->enabled || key->fd < 0) {
        return 0;
    }

    int pressed = 0;
    while (1) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(key->fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int ready = select(key->fd + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) {
            break;
        }

        char ch;
        ssize_t got = read(key->fd, &ch, 1);
        if (got <= 0) {
            break;
        }
        if (ch == 'r' || ch == 'R') {
            pressed = 1;
        }
    }
    return pressed;
#endif
}

static void handle_reset_key(void *userdata) {
    ResetControlContext *ctx = userdata;
    if (ctx && reset_key_pressed(ctx->reset_key)) {
        scatterfix_reset(ctx->engine);
        fprintf(stderr, "[control] action=reset_stream_state key=R\n");
    }
}

static int write_packet(void *userdata, const uint8_t *data, size_t size) {
    return size == TS_PACKET_SIZE && ts_output_write_packet(userdata, data);
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
    /* The recovered-view CLI shares the in-process C recovery library. */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--contest-mode") ||
            (!strcmp(argv[i], "--mode") && i+1 < argc && !strcmp(argv[i+1], "contest")))
            return sf_contest_cli(argc, argv);
    }
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
        fprintf(stderr,
                "[player] Run on the receiving machine: cvlc 'udp://@:%s' --demux=ts --network-caching=300 --avcodec-hw=none\n",
                strrchr(opt.udp_output_host_port, ':') + 1);
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

    ScatterfixConfig config;
    scatterfix_config_init(&config);
    config.psi_interval_ms = opt.psi_interval_ms;
    config.passthrough = opt.passthrough;
    config.sanitize = opt.sanitize;
    config.clean_ts = opt.clean_ts;
    config.drop_audio = opt.drop_audio;
    config.smooth_video_cc = opt.smooth_video_cc;
    config.smooth_timestamps = opt.smooth_timestamps;
    config.verbose = opt.verbose;
    config.dry_run = opt.dry_run;
    config.ml_enabled = opt.ml_enabled;
    config.contest_mode = opt.contest_mode;
    Scatterfix *engine = scatterfix_create(&config, write_packet, &output);
    if (!engine) {
        fprintf(stderr, "tsscatterfix: cannot allocate engine\n");
        ts_output_close(&output);
        ts_input_close(&input);
        if (json_status_file != stderr) fclose(json_status_file);
        if (out_file != stdout) fclose(out_file);
        if (in_file != stdin) fclose(in_file);
        ts_io_cleanup();
        return 1;
    }
    scatterfix_load_state(engine, opt.ml_state_path);
    uint8_t raw[TS_PACKET_SIZE];
    ResetKeyControl reset_key;
    reset_key_open(&reset_key, opt.verbose);
    ResetControlContext reset_control = { &reset_key, engine };
    ts_input_set_poll_callback(&input, handle_reset_key, &reset_control);
    uint64_t stats_index = 0;
    int failed = 0;
    uint64_t bytes_this_pass = 0;

    while (1) {
        size_t got = ts_input_read(&input, raw, sizeof(raw));
        if (!got) {
            if (input.kind == TS_IO_FILE && ferror(in_file)) { failed = 1; break; }
            if (opt.loop_input && opt.input_path && !opt.udp_input_port) {
                if (!bytes_this_pass) break;
                if (!scatterfix_finish(engine)) { failed = 1; break; }
                if (fseek(in_file, 0, SEEK_SET) != 0) {
                    fprintf(stderr, "tsscatterfix: cannot loop input '%s': %s\n", opt.input_path, strerror(errno));
                    failed = 1; break;
                }
                bytes_this_pass = 0;
                if (opt.verbose) {
                    fprintf(stderr, "[io] loop_input path=%s\n", opt.input_path);
                }
                continue;
            }
            break;
        }
        bytes_this_pass += got;
        handle_reset_key(&reset_control);
        if (!scatterfix_feed(engine, raw, got)) {
            fprintf(stderr, "tsscatterfix: processing/write failed\n");
            failed = 1;
            break;
        }
        if (opt.stats_interval > 0 && scatterfix_stats(engine)->packets_read % opt.stats_interval == 0) {
            stats_print(stderr, scatterfix_stats(engine), scatterfix_service(engine), ++stats_index);
            scatterfix_print_roles(engine, stderr);
            if (opt.json_status) {
                scatterfix_print_status(engine, json_status_file, stats_index);
                fflush(json_status_file);
            }
        }
        if (opt.max_packets > 0 && scatterfix_stats(engine)->packets_read >= opt.max_packets) {
            break;
        }
    }

    if (!scatterfix_finish(engine)) failed = 1;
    if (opt.stats_interval > 0 || opt.verbose) {
        stats_print(stderr, scatterfix_stats(engine), scatterfix_service(engine), ++stats_index);
        scatterfix_print_roles(engine, stderr);
    }
    if (opt.json_status) {
        scatterfix_print_status(engine, json_status_file, stats_index);
        fflush(json_status_file);
    }
    if (scatterfix_save_state(engine, opt.ml_state_path) < 0) {
        fprintf(stderr, "tsscatterfix: cannot save ML state '%s': %s\n", opt.ml_state_path, strerror(errno));
    }

    if (!ts_output_flush(&output)) {
        fprintf(stderr, "tsscatterfix: output flush failed\n");
        failed = 1;
    }
    scatterfix_destroy(engine);
    reset_key_close(&reset_key);
    ts_output_close(&output);
    ts_input_close(&input);
    if (json_status_file && json_status_file != stderr) fclose(json_status_file);
    if (out_file && out_file != stdout) fclose(out_file);
    if (in_file && in_file != stdin) fclose(in_file);
    ts_io_cleanup();
    return failed || ferror(stdout) ? 1 : 0;
}
