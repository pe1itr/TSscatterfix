#ifndef SCATTERFIX_RECOVERY_H
#define SCATTERFIX_RECOVERY_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SF_FEATURE_COUNT 11
extern const char *const sf_feature_names[SF_FEATURE_COUNT];
typedef enum { SF_CODEC_AUTO, SF_CODEC_H264, SF_CODEC_H265 } SfCodec;
typedef struct SfRecovery SfRecovery;
typedef struct {
    double means[SF_FEATURE_COUNT], scales[SF_FEATURE_COUNT];
    double weights[SF_FEATURE_COUNT + 1];
    int enabled;
} SfRankModel;
typedef struct {
    uint64_t index;
    SfCodec codec;
    double values[SF_FEATURE_COUNT];
    double score;
} SfFeatures;
/* RGB24, top row first. Pixels are owned by the library and valid only during
 * the callback. Copy them for deferred GUI updates. stride is bytes per row.
 * Callbacks execute on the thread calling feed/finish/analyze, not a GUI thread. */
typedef struct {
    const uint8_t *pixels;
    int width, height, stride;
    double score, quality;
    uint64_t version;
} SfImage;
typedef void (*SfImageCallback)(void *userdata, const SfImage *image);
typedef int (*SfCandidateCallback)(void *userdata, const uint8_t *data, size_t size,
                                   const SfFeatures *features, int voted);
typedef int (*SfFeaturesCallback)(void *userdata, const SfFeatures *features);
typedef enum {
    SF_WAIT_SERVICE, SF_WAIT_HEADERS, SF_WAIT_KEYFRAME,
    SF_READY_TO_DECODE, SF_WAIT_USABLE_IMAGE, SF_IMAGE_READY
} SfRecoveryPhase;
typedef struct {
    char service_name[256], provider_name[256];
    unsigned service_id;
    int video_pid;
    SfCodec codec;
    SfRecoveryPhase phase;
    unsigned progress_percent;     /* evidence-based estimate, not ETA */
    unsigned headers_found, headers_required;
    uint64_t bytes_received, packets_received, access_units, keyframes;
    uint64_t decode_attempts, decoded_images, frame_version;
    uint64_t discarded_bytes, oversized_units;
    size_t retained_bytes;
    double best_score, image_quality;
} SfRecoveryStatus;
/* Borrowed snapshot, invoked once per feed/finish/analyze. */
typedef void (*SfStatusCallback)(void *userdata, const SfRecoveryStatus *status);
typedef struct {
    int video_pid;                 /* -1: detect from PAT/PMT */
    SfCodec codec;
    size_t max_bytes;              /* total retained PES/candidate budget */
    unsigned max_candidates;
    int vote, decode;
    int analyze_on_feed;           /* 1: live; 0: rank the full capture at finish */
    double min_image_quality;
    SfRankModel model;             /* copied; no caller-owned pointer */
    SfImageCallback image;
    SfCandidateCallback candidate;
    SfFeaturesCallback features;
    SfStatusCallback status;
    void *userdata;
} SfRecoveryConfig;

void sf_recovery_config_init(SfRecoveryConfig *config);
SfRecovery *sf_recovery_create(const SfRecoveryConfig *config);
/* Feed arbitrary TS byte chunks. Return 1 on success, 0 on failure.
 * Decode is in-process via FFmpeg. No files, processes, sockets or window needed.
 * Instances are independent; serialize calls to a given instance. */
int sf_recovery_feed(SfRecovery *ctx, const uint8_t *data, size_t size);
int sf_recovery_finish(SfRecovery *ctx);
/* Try the current incomplete PES and retained candidates without consuming the
 * PES. Call on a timer/batch boundary when reception stalls before a new PES.
 * New bytes can still extend that candidate on the next feed. */
int sf_recovery_analyze(SfRecovery *ctx);
int sf_recovery_get_status(const SfRecovery *ctx, SfRecoveryStatus *status);
void sf_recovery_reset(SfRecovery *ctx);
void sf_recovery_destroy(SfRecovery *ctx);
int sf_rank_model_load(SfRankModel *model, const char *path);
double sf_rank_score(const SfRankModel *model, const SfFeatures *features);
#ifdef __cplusplus
}
#endif
#endif
