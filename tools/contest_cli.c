#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "contest_cli.h"
#include "recovery.h"
#include "media.h"
#include "io.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

typedef struct {
    const char *directory, *source;
    FILE *features, *status;
    int json, failed, decode_candidates;
    unsigned candidate_count;
    uint64_t last_packets;
    unsigned last_progress;
    SfImage best;
    mtx_t lock;
    SfVideoOutput *output;
    atomic_int stop, output_failed;
    int fps;
} App;
static volatile sig_atomic_t interrupted;
static void stop_signal(int signal_number) { (void)signal_number; interrupted = 1; }
static int mkdirs(const char *path) {
    if (!path || !*path || strlen(path) >= 4096) return 0;
    char copy[4096]; snprintf(copy,sizeof(copy),"%s",path);
    for (char *p = copy+1;; ++p) {
        if (*p == '/' || !*p) {
            char saved = *p; *p = 0;
#ifdef _WIN32
            int r = _mkdir(copy);
#else
            int r = mkdir(copy,0777);
#endif
            if (r && errno != EEXIST) return 0;
            *p = saved; if (!saved) break;
        }
    }
    return 1;
}
static int path_join(char *path, size_t size, const char *dir, const char *name) {
    int n = snprintf(path,size,"%s/%s",dir,name); return n >= 0 && (size_t)n < size;
}
static void csv_string(FILE *file, const char *value) {
    fputc('"',file);
    for (; *value; ++value) { if (*value == '"') fputc('"',file); fputc(*value,file); }
    fputc('"',file);
}
static void json_string(FILE *file, const char *value) {
    fputc('"',file);
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') { fputc('\\',file); fputc(c,file); }
        else if (c < 32 || c >= 127) fprintf(file,"\\u%04x",c);
        else fputc(c,file);
    }
    fputc('"',file);
}
static int features(void *userdata, const SfFeatures *f) {
    App *app = userdata;
    if (!app->features) return 1;
    csv_string(app->features,app->source);
    fprintf(app->features,",%" PRIu64 ",%.17g",f->index,f->score);
    for (int i = 0; i < SF_FEATURE_COUNT; ++i) fprintf(app->features,",%.17g",f->values[i]);
    fputc('\n',app->features); return !ferror(app->features);
}
static int candidate(void *userdata, const uint8_t *data, size_t size, const SfFeatures *f, int voted) {
    App *app = userdata;
    if (!app->directory) return 1;
    char name[512], path[4096];
    snprintf(name,sizeof(name),"%s_%04u_au_%06" PRIu64 ".%s",voted ? "voted" : "candidate",++app->candidate_count,f->index,f->codec == SF_CODEC_H265 ? "h265" : "h264");
    if (!path_join(path,sizeof(path),app->directory,name)) return 0;
    FILE *file = fopen(path,"wb"); if (!file) return 0;
    int ok = fwrite(data,1,size,file) == size; if (fclose(file)) ok = 0;
    fprintf(stderr,"[fragment] candidate=%s score=%.3f bytes=%zu\n",name,f->score,size);
    if (ok && app->decode_candidates) {
        SfImage image = {0};
        if (sf_image_decode(data,size,f->codec,&image)) {
            char *extension = strrchr(path,'.'); strcpy(extension,".png");
            ok = sf_image_write_png(path,&image); sf_image_free(&image);
        }
    }
    return ok;
}
static void image_ready(void *userdata, const SfImage *image) {
    App *app = userdata;
    size_t size = (size_t)image->height * image->stride;
    uint8_t *copy = malloc(size);
    if (!copy) { app->failed = 1; return; }
    memcpy(copy,image->pixels,size);
    mtx_lock(&app->lock);
    sf_image_free(&app->best); app->best = *image; app->best.pixels = copy;
    mtx_unlock(&app->lock);
    if (app->directory) {
        char path[4096];
        if (!path_join(path,sizeof(path),app->directory,"best.png") || !sf_image_write_png(path,image)) app->failed = 1;
    }
    fprintf(stderr,"[live] best_update score=%.3f quality=%.3f size=%dx%d frame_version=%" PRIu64 "\n",
            image->score,image->quality,image->width,image->height,image->version);
}
static void status_changed(void *userdata, const SfRecoveryStatus *s) {
    App *app = userdata;
    if (s->packets_received-app->last_packets < 500 && s->progress_percent == app->last_progress) return;
    app->last_packets = s->packets_received; app->last_progress = s->progress_percent;
    if (app->json) {
        FILE *f = app->status;
        fprintf(f,"{\"service_name\":"); json_string(f,s->service_name);
        fprintf(f,",\"provider_name\":"); json_string(f,s->provider_name);
        fprintf(f,",\"service_id\":%u,\"video_pid\":%d,\"codec\":%d,\"phase\":%d,\"progress_percent\":%u,\"progress_is_estimate\":true,\"headers_found\":%u,\"headers_required\":%u,\"packets_received\":%" PRIu64 ",\"bytes_received\":%" PRIu64 ",\"access_units\":%" PRIu64 ",\"keyframes\":%" PRIu64 ",\"decode_attempts\":%" PRIu64 ",\"decoded_images\":%" PRIu64 ",\"frame_version\":%" PRIu64 ",\"retained_bytes\":%zu,\"image_quality\":%.6f}\n",
            s->service_id,s->video_pid,s->codec,s->phase,s->progress_percent,s->headers_found,s->headers_required,
            s->packets_received,s->bytes_received,s->access_units,s->keyframes,s->decode_attempts,s->decoded_images,s->frame_version,s->retained_bytes,s->image_quality);
        if (fflush(f)) app->failed = 1;
    } else fprintf(stderr,"[recovery] progress=%u%% phase=%d packets=%" PRIu64 " headers=%u/%u keys=%" PRIu64 " service=%s provider=%s\n",
        s->progress_percent,s->phase,s->packets_received,s->headers_found,s->headers_required,s->keyframes,s->service_name,s->provider_name);
}
static double monotonic_seconds(void) {
    struct timespec now;
#ifdef _WIN32
    timespec_get(&now, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif
    return (double)now.tv_sec + now.tv_nsec/1e9;
}
static int output_thread(void *userdata) {
    App *app = userdata;
    struct timespec pause = {0,10000000};
    double next = monotonic_seconds(), interval = 1.0/app->fps;
    while (!atomic_load(&app->stop)) {
        double now = monotonic_seconds();
        if (now >= next) {
            mtx_lock(&app->lock);
            int ok = sf_video_output_send(app->output,&app->best);
            mtx_unlock(&app->lock);
            if (!ok) { atomic_store(&app->output_failed,1); break; }
            next += interval;
            if (next < now) next = now + interval;
        }
        thrd_sleep(&pause,NULL);
    }
    return 0;
}
static int integer(const char *text, long min, long max, long *value) {
    char *end; errno = 0; long n = strtol(text,&end,0);
    if (!*text || *end || errno || n < min || n > max) return 0;
    *value = n; return 1;
}
static void usage(void) {
    fprintf(stderr,
        "Usage: scatterfix-contest [recover|live|probe] [options]\n"
        "  --input PATH          TS capture (repeat for multiple captures; default stdin)\n"
        "  --udp-input PORT      Receive live UDP TS\n"
        "  --output-dir PATH     Save candidates and best.png (optional)\n"
        "  --work-dir PATH       Same as --output-dir, selects live mode\n"
        "  --video-pid N         Force video PID; default PAT/PMT detection\n"
        "  --codec auto|h264|h265\n"
        "  --decode              Decode candidates to PNG as well as best.png\n"
        "  --no-decode           Extract only\n"
        "  --vote                Also try weighted byte voting\n"
        "  --max-candidates N    Number to analyze per pass (default 12)\n"
        "  --max-mb N            PES/candidate memory budget (default 8)\n"
        "  --min-image-quality N Acceptance threshold, 0..1 (default .15)\n"
        "  --features-csv PATH   Export ranking features\n"
        "  --ml-model PATH       Load linear JSON ranking model (--model alias)\n"
        "  --json-status [--json-status-output PATH]\n"
        "  --udp-output HOST:PORT  Publish best still as MPEG-TS; keeps running at EOF\n"
        "  --output PATH         Write recovered MPEG-TS to file\n"
        "  --output-size WxH     Default 640x360\n"
        "  --output-fps N        Integer 1..60, default 1\n"
        "  --max-packets N       Stop receiving after N packets\n"
        "  --help\n");
}
int sf_contest_cli(int argc, char **argv) {
    SfRecoveryConfig config; sf_recovery_config_init(&config);
    App app = {0}; app.status = stderr; app.fps = 1; app.last_progress = 999;
    const char *inputs[256]; int input_count = 0;
    const char *udp_input = NULL, *udp_output = NULL, *output_path = NULL, *model = NULL, *features_path = NULL, *status_path = NULL;
    int live = 0, width = 640, height = 360; long max_packets = 0;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i]; long n;
        if (!strcmp(arg,"live") || !strcmp(arg,"--contest-mode")) live = 1;
        else if (!strcmp(arg,"recover")) live = 0;
        else if (!strcmp(arg,"probe")) config.decode = 0;
        else if (!strcmp(arg,"--help")) { usage(); return 0; }
        else if (!strcmp(arg,"--decode")) app.decode_candidates = 1;
        else if (!strcmp(arg,"--no-decode") || !strcmp(arg,"--dry-run")) config.decode = 0;
        else if (!strcmp(arg,"--vote")) config.vote = 1;
        else if (!strcmp(arg,"--json-status")) app.json = 1;
        else if (!strcmp(arg,"--verbose")) { /* status is always logged */ }
        else {
            if (++i == argc) goto bad;
            const char *value = argv[i];
            if (!strcmp(arg,"--input")) { if (input_count == 256) goto bad; inputs[input_count++] = value; }
            else if (!strcmp(arg,"--udp-input")) { udp_input = value; live = 1; }
            else if (!strcmp(arg,"--udp-output")) udp_output = value;
            else if (!strcmp(arg,"--output")) output_path = value;
            else if (!strcmp(arg,"--output-dir") || !strcmp(arg,"--work-dir")) { app.directory = value; if (!strcmp(arg,"--work-dir")) live = 1; }
            else if (!strcmp(arg,"--mode")) { if (strcmp(value,"contest")) goto bad; live = 1; }
            else if (!strcmp(arg,"--codec")) {
                config.codec = !strcmp(value,"auto") ? SF_CODEC_AUTO : !strcmp(value,"h264") ? SF_CODEC_H264 : !strcmp(value,"h265") ? SF_CODEC_H265 : -1;
                if ((int)config.codec < 0) goto bad;
            } else if (!strcmp(arg,"--video-pid")) { if (!integer(value,0,8190,&n)) goto bad; config.video_pid = (int)n; }
            else if (!strcmp(arg,"--max-candidates")) { if (!integer(value,1,256,&n)) goto bad; config.max_candidates = (unsigned)n; }
            else if (!strcmp(arg,"--max-mb")) { if (!integer(value,1,256,&n)) goto bad; config.max_bytes = (size_t)n*1024*1024; }
            else if (!strcmp(arg,"--max-packets")) { if (!integer(value,1,2147483647,&max_packets)) goto bad; }
            else if (!strcmp(arg,"--output-fps")) { if (!integer(value,1,60,&n)) goto bad; app.fps = (int)n; }
            else if (!strcmp(arg,"--output-size")) {
                char tail; if (sscanf(value,"%dx%d%c",&width,&height,&tail) != 2 || width < 2 || height < 2 || width > 8192 || height > 8192 || (width&1) || (height&1)) goto bad;
            } else if (!strcmp(arg,"--min-image-quality")) {
                char *end; config.min_image_quality = strtod(value,&end);
                if (!*value || *end || !isfinite(config.min_image_quality) || config.min_image_quality < 0 || config.min_image_quality > 1) goto bad;
            } else if (!strcmp(arg,"--model") || !strcmp(arg,"--ml-model")) model = value;
            else if (!strcmp(arg,"--features-csv")) features_path = value;
            else if (!strcmp(arg,"--json-status-output")) { app.json = 1; status_path = value; }
            else goto bad;
        }
    }
    if ((udp_input && input_count) || (udp_output && output_path)) goto bad;
    if (model && !sf_rank_model_load(&config.model,model)) { fprintf(stderr,"Invalid ranking model: %s\n",model); return 1; }
    if (app.directory && !mkdirs(app.directory)) { perror(app.directory); return 1; }
    if (mtx_init(&app.lock,mtx_plain) != thrd_success) return 1;
    atomic_init(&app.stop,0); atomic_init(&app.output_failed,0);
    SfRecovery *recovery = NULL; thrd_t thread; int thread_started = 0, io_started = 0;
    if (status_path && !(app.status = fopen(status_path,"w"))) goto fail;
    if (features_path) {
        app.features = fopen(features_path,"w"); if (!app.features) goto fail;
        fprintf(app.features,"source,access_unit,score");
        for (int i = 0; i < SF_FEATURE_COUNT; ++i) fprintf(app.features,",%s",sf_feature_names[i]);
        fputc('\n',app.features);
    }
    config.image = image_ready; config.candidate = candidate; config.features = features;
    config.status = status_changed; config.userdata = &app; config.analyze_on_feed = live;
    recovery = sf_recovery_create(&config); if (!recovery) goto fail;
    if (!(io_started = ts_io_startup())) goto fail;
    if (udp_output || output_path) {
        char url[4096];
        if (udp_output) {
            int n = snprintf(url,sizeof(url),"udp://%s?pkt_size=1316",udp_output); if (n < 0 || (size_t)n >= sizeof(url)) goto fail;
        } else if (snprintf(url,sizeof(url),"%s",output_path) >= (int)sizeof(url)) goto fail;
        app.output = sf_video_output_open(url,width,height,app.fps); if (!app.output) goto fail;
        app.best.width = width; app.best.height = height; app.best.stride = width*3;
        app.best.pixels = calloc((size_t)height,app.best.stride); if (!app.best.pixels) goto fail;
        if (udp_output) {
            if (thrd_create(&thread,output_thread,&app) != thrd_success) goto fail;
            thread_started = 1;
        }
    }
    interrupted = 0;
    void (*previous_int)(int) = signal(SIGINT,stop_signal);
    void (*previous_term)(int) = signal(SIGTERM,stop_signal);
    if (!input_count) inputs[input_count++] = NULL;
    for (int index = 0; index < input_count && !interrupted && !app.failed; ++index) {
        FILE *file = inputs[index] ? fopen(inputs[index],"rb") : stdin;
        TsInput input; ts_input_init_file(&input,file);
        if (!file || (udp_input && !ts_input_init_udp(&input,udp_input))) { if (file && file != stdin) fclose(file); app.failed = 1; break; }
        app.source = inputs[index] ? inputs[index] : "stdin";
        uint8_t buffer[65536];
        while (!interrupted && !app.failed && !atomic_load(&app.output_failed)) {
            size_t capacity = sizeof(buffer);
            SfRecoveryStatus s; sf_recovery_get_status(recovery,&s);
            if (max_packets) {
                uint64_t remaining = (uint64_t)max_packets*188;
                if (s.bytes_received >= remaining) break;
                remaining -= s.bytes_received; if (remaining < capacity) capacity = (size_t)remaining;
            }
            int got;
            if (udp_input) {
                fd_set fds; FD_ZERO(&fds); FD_SET(input.sock,&fds);
                struct timeval timeout = {0,100000};
                int ready = select((int)input.sock+1,&fds,NULL,NULL,&timeout);
                if (!ready || (ready < 0 && errno == EINTR)) continue;
                if (ready < 0) { app.failed = 1; break; }
                got = recv(input.sock,(char *)buffer,(int)capacity,0);
            } else {
#ifdef _WIN32
                got = (int)fread(buffer,1,capacity,file);
#else
                fd_set fds; FD_ZERO(&fds); int fd = fileno(file); FD_SET(fd,&fds);
                struct timeval timeout = {0,100000};
                int ready = select(fd+1,&fds,NULL,NULL,&timeout);
                if (!ready || (ready < 0 && errno == EINTR)) continue;
                if (ready < 0) { app.failed = 1; break; }
                got = (int)read(fd,buffer,capacity);
#endif
            }
            if (got < 0) { if (errno == EINTR) continue; app.failed = 1; break; }
            if (!got) break;
            if (!sf_recovery_feed(recovery,buffer,(size_t)got)) { app.failed = 1; break; }
            if (live && !sf_recovery_analyze(recovery)) { app.failed = 1; break; }
        }
        if (!sf_recovery_finish(recovery)) app.failed = 1;
        ts_input_close(&input); if (file != stdin && fclose(file)) app.failed = 1;
    }
    if (output_path && !app.failed && !sf_video_output_send(app.output,&app.best)) app.failed = 1;
    SfRecoveryStatus final_status; sf_recovery_get_status(recovery,&final_status);
    app.last_progress = 999; status_changed(&app,&final_status);
    if (udp_output && !max_packets) {
        struct timespec pause = {0,100000000};
        while (!interrupted && !app.failed && !atomic_load(&app.output_failed)) thrd_sleep(&pause,NULL);
    }
    signal(SIGINT,previous_int); signal(SIGTERM,previous_term);
    goto done;
bad:
    usage(); return 2;
fail:
    fprintf(stderr,"scatterfix-contest: initialization failed\n"); app.failed = 1;
done:
    atomic_store(&app.stop,1);
    if (thread_started) thrd_join(thread,NULL);
    if (!sf_video_output_close(app.output)) app.failed = 1;
    sf_image_free(&app.best); sf_recovery_destroy(recovery);
    if (app.features && fclose(app.features)) app.failed = 1;
    if (app.status && app.status != stderr && fclose(app.status)) app.failed = 1;
    if (io_started) ts_io_cleanup();
    mtx_destroy(&app.lock);
    return app.failed || atomic_load(&app.output_failed) ? 1 : 0;
}
