#include "recovery.h"
#include "media.h"
#include "psi.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

const char *const sf_feature_names[SF_FEATURE_COUNT] = {
    "packet_confidence", "packets", "slice_bytes", "cc_errors", "missing_by_cc",
    "out_of_order", "bad_pes_starts", "low_entropy_packets", "high_entropy_packets", "idr_bytes", "has_idr"
};
typedef struct {
    uint8_t *data;
    size_t size, key_offset, key_size;
    SfFeatures features;
    unsigned emitted;
} Candidate;
struct SfRecovery {
    SfRecoveryConfig config;
    SfRecoveryStatus status;
    PsiContext psi;
    uint8_t pending[188]; size_t pending_size;
    uint8_t *pes; size_t pes_size;
    int dropping, failed, last_cc, pid;
    SfCodec codec;
    SfFeatures current;
    uint8_t *headers[3]; size_t header_sizes[3];
    unsigned header_version;
    Candidate candidates[256]; size_t count, retained;
    int dirty;
    size_t preview_size;
};

void sf_recovery_config_init(SfRecoveryConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->video_pid = -1; config->max_bytes = 8*1024*1024;
    config->max_candidates = 12; config->min_image_quality = .15;
    config->decode = 1; config->analyze_on_feed = 1;
}
static void update_status(SfRecovery *ctx) {
    SfRecoveryStatus *s = &ctx->status;
    memcpy(s->service_name, ctx->psi.service_name, sizeof(s->service_name));
    memcpy(s->provider_name, ctx->psi.provider_name, sizeof(s->provider_name));
    s->service_id = ctx->psi.service_id; s->video_pid = ctx->pid; s->codec = ctx->codec;
    s->headers_required = ctx->codec == SF_CODEC_H265 ? 3 : 2;
    s->headers_found = 0;
    for (unsigned i = 0; i < s->headers_required; ++i) s->headers_found += ctx->header_sizes[i] != 0;
    s->retained_bytes = ctx->retained + ctx->pes_size;
    s->phase = SF_WAIT_SERVICE; s->progress_percent = 0;
    if (ctx->pid >= 0) { s->phase = SF_WAIT_HEADERS; s->progress_percent = 20 + 40*s->headers_found/s->headers_required; }
    if (ctx->pid >= 0 && s->headers_found == s->headers_required) {
        s->phase = SF_WAIT_KEYFRAME; s->progress_percent = 60;
        if (ctx->count) { s->phase = SF_READY_TO_DECODE; s->progress_percent = 80; }
        if (ctx->count && s->decode_attempts) { s->phase = SF_WAIT_USABLE_IMAGE; s->progress_percent = 90; }
    }
    if (s->frame_version) { s->phase = SF_IMAGE_READY; s->progress_percent = 100; }
}
int sf_recovery_get_status(const SfRecovery *ctx, SfRecoveryStatus *status) {
    if (!ctx || !status) return 0;
    *status = ctx->status; return 1;
}
static void notify_status(SfRecovery *ctx) {
    update_status(ctx);
    if (ctx->config.status) ctx->config.status(ctx->config.userdata, &ctx->status);
}
static void clear_candidates(SfRecovery *ctx) {
    for (size_t i = 0; i < ctx->count; ++i) free(ctx->candidates[i].data);
    ctx->count = ctx->retained = 0;
    for (int i = 0; i < 3; ++i) {
        free(ctx->headers[i]); ctx->headers[i] = NULL; ctx->header_sizes[i] = 0;
    }
    ctx->header_version = 0; ctx->pes_size = 0; ctx->dropping = 0;
    ctx->preview_size = 0;
    memset(&ctx->current, 0, sizeof(ctx->current)); ctx->last_cc = -1;
}
void sf_recovery_reset(SfRecovery *ctx) {
    if (!ctx) return;
    clear_candidates(ctx); psi_init(&ctx->psi);
    memset(&ctx->status, 0, sizeof(ctx->status));
    ctx->status.best_score = -DBL_MAX;
    ctx->pid = ctx->config.video_pid;
    ctx->codec = ctx->config.codec == SF_CODEC_AUTO ? SF_CODEC_H264 : ctx->config.codec;
    ctx->pending_size = 0; ctx->preview_size = 0; ctx->failed = ctx->dirty = 0;
    update_status(ctx);
}
SfRecovery *sf_recovery_create(const SfRecoveryConfig *config) {
    SfRecoveryConfig c; sf_recovery_config_init(&c); if (config) c = *config;
    if (c.video_pid < -1 || c.video_pid >= 8191 || c.codec < SF_CODEC_AUTO || c.codec > SF_CODEC_H265 ||
        c.max_bytes < 4096 || c.max_bytes > 256u*1024*1024 || !c.max_candidates || c.max_candidates > 256 ||
        !isfinite(c.min_image_quality) || c.min_image_quality < 0 || c.min_image_quality > 1) return NULL;
    if (c.model.enabled) {
        if (!isfinite(c.model.weights[0])) return NULL;
        for (int i = 0; i < SF_FEATURE_COUNT; ++i)
            if (!isfinite(c.model.means[i]) || !isfinite(c.model.scales[i]) ||
                !isfinite(c.model.weights[i+1])) return NULL;
    }
    SfRecovery *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->config = c; ctx->pes = malloc(c.max_bytes/2);
    if (!ctx->pes) { free(ctx); return NULL; }
    sf_recovery_reset(ctx); return ctx;
}
void sf_recovery_destroy(SfRecovery *ctx) {
    if (ctx) { clear_candidates(ctx); free(ctx->pes); free(ctx); }
}
double sf_rank_score(const SfRankModel *model, const SfFeatures *features) {
    if (!model || !model->enabled) return features->score;
    double score = model->weights[0];
    for (int i = 0; i < SF_FEATURE_COUNT; ++i) {
        double scale = model->scales[i] ? model->scales[i] : 1;
        score += model->weights[i+1] * (features->values[i]-model->means[i])/scale;
    }
    return score;
}

static size_t start_code(const uint8_t *data, size_t size, size_t at, size_t *prefix) {
    for (size_t i = at; i + 3 < size; ++i) {
        if (!data[i] && !data[i+1]) {
            if (data[i+2] == 1) { *prefix = 3; return i; }
            if (i+4 < size && !data[i+2] && data[i+3] == 1) { *prefix = 4; return i; }
        }
    }
    return size;
}
static int emit_candidate(SfRecovery *ctx, const uint8_t *data, size_t size, const SfFeatures *features, int voted) {
    if (ctx->config.candidate && !ctx->config.candidate(ctx->config.userdata, data, size, features, voted)) return 0;
    if (!ctx->config.decode || (ctx->status.frame_version && features->score <= ctx->status.best_score)) return 1;
    SfImage image = {0}; ctx->status.decode_attempts++;
    if (!sf_image_decode(data, size, features->codec, &image)) return 1;
    ctx->status.decoded_images++;
    if (image.quality >= ctx->config.min_image_quality) {
        image.score = features->score; image.version = ++ctx->status.frame_version;
        ctx->status.best_score = image.score; ctx->status.image_quality = image.quality;
        update_status(ctx);
        if (ctx->config.image) ctx->config.image(ctx->config.userdata, &image);
    }
    sf_image_free(&image); return 1;
}
static int analyze(SfRecovery *ctx) {
    unsigned required = ctx->codec == SF_CODEC_H265 ? 3 : 2;
    size_t header_size = 0;
    for (unsigned h = 0; h < required; ++h) {
        if (!ctx->header_sizes[h]) return 1;
        header_size += ctx->header_sizes[h];
    }
    size_t order[256];
    for (size_t i = 0; i < ctx->count; ++i) {
        size_t j = i;
        while (j && ctx->candidates[order[j-1]].features.score < ctx->candidates[i].features.score) { order[j] = order[j-1]; --j; }
        order[j] = i;
    }
    unsigned emitted = 0;
    for (size_t i = 0; i < ctx->count && emitted < ctx->config.max_candidates; ++i) {
        Candidate *c = &ctx->candidates[order[i]];
        if (c->emitted == ctx->header_version) continue;
        size_t n = header_size + c->size;
        uint8_t *data = malloc(n); if (!data) return 0;
        size_t at = 0;
        for (unsigned h = 0; h < required; ++h) { memcpy(data+at, ctx->headers[h], ctx->header_sizes[h]); at += ctx->header_sizes[h]; }
        memcpy(data+at, c->data, c->size);
        int ok = emit_candidate(ctx, data, n, &c->features, 0); free(data);
        if (!ok) return 0;
        c->emitted = ctx->header_version; ++emitted;
    }
    if (ctx->config.vote && ctx->dirty) {
        for (size_t i = 0; i < ctx->count; ++i) {
            Candidate *c = &ctx->candidates[order[i]];
            size_t bucket = (c->key_size+93)/188, members[256], count = 0, max = 0;
            int earlier = 0;
            for (size_t j = 0; j < i; ++j) if ((ctx->candidates[order[j]].key_size+93)/188 == bucket) earlier = 1;
            if (earlier) continue;
            for (size_t j = i; j < ctx->count; ++j) {
                Candidate *other = &ctx->candidates[order[j]];
                if ((other->key_size+93)/188 == bucket) {
                    members[count++] = order[j]; if (other->key_size > max) max = other->key_size;
                }
            }
            if (count < 2) continue;
            uint8_t *data = malloc(header_size+max); if (!data) return 0;
            size_t at = 0;
            for (unsigned h = 0; h < required; ++h) { memcpy(data+at, ctx->headers[h], ctx->header_sizes[h]); at += ctx->header_sizes[h]; }
            for (size_t pos = 0; pos < max; ++pos) {
                double counts[256] = {0}; unsigned best = 0; double best_weight = -1;
                for (size_t j = 0; j < count; ++j) {
                    Candidate *other = &ctx->candidates[members[j]];
                    if (pos >= other->key_size) continue;
                    double base = other->features.score * (ctx->config.model.enabled ? 1024 : 1);
                    double weight = fmax(1, floor((base+4096)*(.5+other->features.values[0])));
                    unsigned value = other->data[other->key_offset+pos];
                    counts[value] += weight;
                }
                /* Stable ties prefer the highest ranked observed byte. */
                for (size_t j = 0; j < count; ++j) {
                    Candidate *other = &ctx->candidates[members[j]];
                    if (pos < other->key_size) {
                        unsigned value = other->data[other->key_offset+pos];
                        if (counts[value] > best_weight) { best = value; best_weight = counts[value]; }
                    }
                }
                data[at+pos] = (uint8_t)best;
            }
            int ok = emit_candidate(ctx, data, header_size+max, &c->features, 1); free(data);
            if (!ok) return 0;
        }
    }
    ctx->dirty = 0; return 1;
}
static int finish_pes(SfRecovery *ctx, int partial);
int sf_recovery_analyze(SfRecovery *ctx) {
    if (!ctx || ctx->failed) return 0;
    if (ctx->pes_size != ctx->preview_size && !ctx->dropping) {
        ctx->preview_size = ctx->pes_size;
        if (!finish_pes(ctx, 1)) ctx->failed = 1;
    }
    if (!ctx->failed && !analyze(ctx)) ctx->failed = 1;
    notify_status(ctx); return !ctx->failed;
}
static void evict_oldest(SfRecovery *ctx) {
    ctx->retained -= ctx->candidates[0].size;
    free(ctx->candidates[0].data);
    memmove(ctx->candidates, ctx->candidates+1, (--ctx->count)*sizeof(Candidate));
}
static int finish_pes(SfRecovery *ctx, int partial) {
    if (!ctx->current.values[1]) return 1;
    SfFeatures f = ctx->current; f.index = ctx->status.access_units; f.codec = ctx->codec;
    if (!partial) ctx->status.access_units++;
    f.values[0] /= f.values[1];
    uint8_t *data = ctx->pes; size_t size = ctx->pes_size;
    if (size >= 9 && !data[0] && !data[1] && data[2] == 1) {
        size_t skip = 9u+data[8]; if (skip <= size) { data += skip; size -= skip; } else size = 0;
    }
    Candidate candidate = {0};
    candidate.data = malloc(size ? size : 1); if (!candidate.data) return 0;
    size_t prefix = 0, at = start_code(data, size, 0, &prefix);
    while (at < size) {
        size_t next_prefix = 0, end = start_code(data, size, at+prefix, &next_prefix);
        size_t n = end-at;
        if (at+prefix < end && (ctx->codec != SF_CODEC_H265 || at+prefix+1 < end)) {
            int type = ctx->codec == SF_CODEC_H265 ? (data[at+prefix]>>1)&63 : data[at+prefix]&31;
            int h = ctx->codec == SF_CODEC_H265 ? type-32 : type-7;
            int header = ctx->codec == SF_CODEC_H265 ? h >= 0 && h < 3 : h >= 0 && h < 2;
            int key = ctx->codec == SF_CODEC_H265 ? type >= 19 && type <= 21 : type == 5;
            int slice = ctx->codec == SF_CODEC_H265 ? type < 32 : type == 1 || type == 5;
            int pass = key || (ctx->codec == SF_CODEC_H265 ? type == 35 || type == 39 || type == 40 : type == 6 || type == 9);
            if (header && n > ctx->header_sizes[h] && n <= 65536) {
                uint8_t *copy = malloc(n);
                if (!copy) { free(candidate.data); return 0; }
                memcpy(copy, data+at, n); free(ctx->headers[h]); ctx->headers[h] = copy; ctx->header_sizes[h] = n;
                ++ctx->header_version; ctx->dirty = 1;
            }
            if (slice) f.values[2] += n;
            if (key) {
                f.values[10] = 1;
                if (!candidate.key_size) { candidate.key_offset = candidate.size; candidate.key_size = n; f.values[9] = n; }
            }
            if (pass) { memcpy(candidate.data+candidate.size, data+at, n); candidate.size += n; }
        }
        at = end; prefix = next_prefix;
    }
    f.score = f.values[2] + trunc((f.values[0]-.5)*f.values[1]*128) - f.values[4]*188 - f.values[3]*64 - f.values[5]*128 - f.values[6]*96;
    if (!partial && ctx->config.features && !ctx->config.features(ctx->config.userdata, &f)) { free(candidate.data); return 0; }
    f.score = sf_rank_score(&ctx->config.model, &f);
    if (f.values[10] && !ctx->dropping) {
        int replacement = 0;
        for (size_t i = 0; i < ctx->count; ++i) if (ctx->candidates[i].features.index == f.index) {
            ctx->retained -= ctx->candidates[i].size; free(ctx->candidates[i].data);
            memmove(ctx->candidates+i, ctx->candidates+i+1, (--ctx->count-i)*sizeof(Candidate));
            replacement = 1; break;
        }
        if (!replacement) ctx->status.keyframes++;
        uint8_t *compact = realloc(candidate.data, candidate.size);
        if (compact) candidate.data = compact;
        candidate.features = f;
        while (ctx->count && (ctx->count == 256 || ctx->retained+candidate.size > ctx->config.max_bytes/2)) evict_oldest(ctx);
        ctx->retained += candidate.size; ctx->candidates[ctx->count++] = candidate; ctx->dirty = 1;
    } else free(candidate.data);
    if (!partial) {
        memset(&ctx->current, 0, sizeof(ctx->current)); ctx->pes_size = 0; ctx->dropping = 0; ctx->preview_size = 0;
    }
    return !ctx->config.analyze_on_feed || analyze(ctx);
}
static int packet(SfRecovery *ctx, const uint8_t *raw) {
    TsPacket p; ctx->status.packets_received++;
    if (!ts_packet_parse(raw, &p)) return 1;
    psi_observe_packet(&ctx->psi, &p, ctx->status.packets_received, 0);
    for (size_t i = 0; i < ctx->psi.video_count; ++i) {
        SfCodec codec = ctx->psi.video_types[i] == 0x24 ? SF_CODEC_H265 : ctx->psi.video_types[i] == 0x1b ? SF_CODEC_H264 : SF_CODEC_AUTO;
        int pid = ctx->psi.video_pids[i];
        if (!codec || (ctx->config.video_pid >= 0 && ctx->config.video_pid != pid) ||
            (ctx->config.codec && ctx->config.codec != codec)) continue;
        if (ctx->pid != pid || ctx->codec != codec) {
            clear_candidates(ctx); ctx->pid = pid; ctx->codec = codec;
            ctx->status.frame_version = 0; ctx->status.best_score = -DBL_MAX;
            ctx->status.decode_attempts = 0; ctx->status.image_quality = 0;
        }
        break;
    }
    if (p.pid != ctx->pid || !p.has_payload) return 1;
    if (p.payload_unit_start && !finish_pes(ctx, 0)) return 0;
    double confidence = .70;
    const uint8_t *payload = raw+p.payload_offset;
    size_t length = p.payload_size;
    if (p.transport_error) confidence -= .70;
    if (p.scrambling) confidence -= .45;
    unsigned counts[256] = {0}; double entropy = 0;
    for (size_t i = 0; i < length; ++i) counts[payload[i]]++;
    for (int i = 0; i < 256; ++i) if (counts[i]) { double v = (double)counts[i]/length; entropy -= v*log2(v); }
    if (entropy < 1) { confidence -= .20; ctx->current.values[7]++; }
    else if (entropy > 7.85) { confidence -= .10; ctx->current.values[8]++; }
    else confidence += .08;
    if (p.payload_unit_start) {
        if (length >= 3 && !payload[0] && !payload[1] && payload[2] == 1) confidence += .16;
        else { confidence -= .30; ctx->current.values[6]++; }
    }
    if (p.discontinuity_indicator) ctx->last_cc = -1;
    if (ctx->last_cc < 0 || p.continuity_counter == ((ctx->last_cc+1)&15)) confidence += .12;
    else {
        ctx->current.values[3]++;
        int delta = (p.continuity_counter-ctx->last_cc-1)&15;
        if (delta >= 1 && delta <= 8) { ctx->current.values[4] += delta; confidence -= fmin(.55, .12*delta); }
        else if (p.continuity_counter == ctx->last_cc) confidence -= .25;
        else { confidence -= .35; ctx->current.values[5]++; }
    }
    ctx->last_cc = p.continuity_counter;
    ctx->current.values[0] += fmax(0, fmin(1, confidence)); ctx->current.values[1]++;
    if (!ctx->dropping && ctx->pes_size+length <= ctx->config.max_bytes/2) {
        memcpy(ctx->pes+ctx->pes_size, payload, length); ctx->pes_size += length;
    } else {
        if (!ctx->dropping) { ctx->status.oversized_units++; ctx->pes_size = 0; }
        ctx->dropping = 1;
    }
    return 1;
}
int sf_recovery_feed(SfRecovery *ctx, const uint8_t *data, size_t size) {
    if (!ctx || (!data && size) || ctx->failed) return 0;
    ctx->status.bytes_received += size;
    while (size) {
        if (!ctx->pending_size && *data != 0x47) { data++; size--; ctx->status.discarded_bytes++; continue; }
        size_t n = 188-ctx->pending_size; if (n > size) n = size;
        memcpy(ctx->pending+ctx->pending_size, data, n); ctx->pending_size += n; data += n; size -= n;
        if (ctx->pending_size == 188) {
            ctx->pending_size = 0;
            if (!packet(ctx, ctx->pending)) { ctx->failed = 1; break; }
        }
    }
    notify_status(ctx); return !ctx->failed;
}
int sf_recovery_finish(SfRecovery *ctx) {
    if (!ctx || ctx->failed) return 0;
    ctx->status.discarded_bytes += ctx->pending_size; ctx->pending_size = 0;
    if (!finish_pes(ctx, 0) || !analyze(ctx)) ctx->failed = 1;
    notify_status(ctx); return !ctx->failed;
}
