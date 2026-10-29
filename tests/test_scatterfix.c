#include "scatterfix.h"
#include "recovery.h"
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
typedef struct { uint8_t data[188*8]; size_t size; int fail; } Sink;
static int write_ts(void *userdata, const uint8_t *data, size_t size) {
    Sink *sink = userdata;
    if (sink->fail) return 0;
    CHECK(sink->size+size <= sizeof(sink->data));
    memcpy(sink->data+sink->size,data,size); sink->size += size; return 1;
}
static void packet(uint8_t *p, unsigned pid, unsigned cc) {
    memset(p,0xff,188); p[0] = 0x47; p[1] = (uint8_t)(pid>>8); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10|cc);
}
static void test_repair(void) {
    ScatterfixConfig config; scatterfix_config_init(&config);
    config.psi_interval_ms = 0; config.passthrough = 1;
    Sink sink = {0}; Scatterfix *ctx = scatterfix_create(&config,write_ts,&sink); CHECK(ctx);
    uint8_t input[188*2+3]; input[0] = 1; input[1] = 2; input[2] = 3;
    packet(input+3,256,0); packet(input+191,256,1);
    for (size_t i = 0; i < sizeof(input); ++i) CHECK(scatterfix_feed(ctx,input+i,1));
    CHECK(scatterfix_finish(ctx)); CHECK(sink.size == 376); CHECK(!memcmp(sink.data,input+3,376));
    CHECK(scatterfix_stats(ctx)->packets_read == 2); CHECK(scatterfix_stats(ctx)->bytes_dropped == 3);
    CHECK(scatterfix_feed(ctx,input+3,10)); CHECK(scatterfix_finish(ctx));
    CHECK(scatterfix_stats(ctx)->bytes_dropped == 13);
    sink.fail = 1; CHECK(!scatterfix_feed(ctx,input+3,188)); CHECK(!scatterfix_feed(ctx,NULL,0));
    scatterfix_reset(ctx); sink.fail = 0; CHECK(scatterfix_feed(ctx,input+3,188));
    scatterfix_destroy(ctx);
    config.passthrough = 0; config.sanitize = 1; sink.size = 0;
    ctx = scatterfix_create(&config,write_ts,&sink); CHECK(ctx);
    input[4] |= 0x80; CHECK(scatterfix_feed(ctx,input+3,188));
    CHECK(sink.size == 188 && sink.data[1] == 0x1f && sink.data[2] == 0xff);
    scatterfix_destroy(ctx);
    CHECK(!scatterfix_create(&config,NULL,NULL));
}
typedef struct {
    unsigned images, statuses, candidates, features;
    int width, height, reject;
    SfRecoveryStatus status;
} Observer;
static void image(void *userdata, const SfImage *im) {
    Observer *o = userdata; CHECK(im->pixels && im->stride >= im->width*3);
    CHECK(im->version == o->images+1); CHECK(im->quality >= .15 && im->quality <= 1);
    o->images++; o->width = im->width; o->height = im->height;
}
static void status(void *userdata, const SfRecoveryStatus *s) {
    Observer *o = userdata; o->statuses++; o->status = *s;
    CHECK(s->progress_percent <= 100);
    CHECK((s->progress_percent == 100) == (s->frame_version != 0));
}
static int candidate(void *userdata, const uint8_t *data, size_t size, const SfFeatures *f, int voted) {
    Observer *o = userdata; (void)voted;
    CHECK(size >= 4 && data[0] == 0 && data[1] == 0); CHECK(f->values[10]);
    o->candidates++; return !o->reject;
}
static int features(void *userdata, const SfFeatures *f) {
    Observer *o = userdata; CHECK(f->values[1] > 0); o->features++; return 1;
}
static void test_capture(const char *path, SfCodec codec, const char *service, const char *provider, size_t chunk) {
    SfRecoveryConfig config; sf_recovery_config_init(&config);
    Observer o = {0}; config.image = image; config.status = status; config.candidate = candidate; config.features = features; config.userdata = &o;
    SfRecovery *ctx = sf_recovery_create(&config); CHECK(ctx);
    FILE *file = fopen(path,"rb"); CHECK(file);
    uint8_t buffer[4096]; size_t n;
    while ((n = fread(buffer,1,chunk,file))) CHECK(sf_recovery_feed(ctx,buffer,n));
    CHECK(!ferror(file)); fclose(file); CHECK(sf_recovery_finish(ctx));
    CHECK(o.images > 0); CHECK(o.candidates > 0 && o.features > 0);
    CHECK(o.status.progress_percent == 100 && o.status.phase == SF_IMAGE_READY);
    CHECK(o.status.codec == codec && o.status.video_pid == 256);
    CHECK(!strcmp(o.status.service_name,service)); CHECK(!strcmp(o.status.provider_name,provider));
    CHECK(o.status.headers_found == (codec == SF_CODEC_H265 ? 3u : 2u));
    CHECK(sf_recovery_get_status(ctx,&o.status));
    sf_recovery_reset(ctx); CHECK(sf_recovery_get_status(ctx,&o.status));
    CHECK(!o.status.frame_version && !o.status.packets_received && !o.status.service_name[0]);
    CHECK(o.status.progress_percent == 0);
    sf_recovery_destroy(ctx);
}
static void pes_packet(uint8_t raw[188], const uint8_t *payload, size_t size, int start, unsigned cc) {
    CHECK(size <= 182); packet(raw,256,cc); raw[1] |= start ? 0x40 : 0;
    raw[3] |= 0x20; raw[4] = (uint8_t)(183-size); raw[5] = 0;
    memcpy(raw+188-size,payload,size);
}
static void test_progress_and_voting(void) {
    SfRecoveryConfig config; sf_recovery_config_init(&config);
    config.video_pid = 256; config.codec = SF_CODEC_H264; config.decode = 0;
    config.vote = 1; config.analyze_on_feed = 0;
    Observer o = {0}; config.status = status; config.candidate = candidate; config.userdata = &o;
    SfRecovery *ctx = sf_recovery_create(&config); CHECK(ctx);
    uint8_t raw[188];
    const uint8_t headers[] = {0,0,1,0xe0,0,0,0x80,0,0, 0,0,0,1,0x67,1,2, 0,0,0,1,0x68,3,4};
    const uint8_t key[] = {0,0,1,0xe0,0,0,0x80,0,0, 0,0,0,1,0x65,1,2,3,4,5};
    pes_packet(raw,headers,sizeof(headers),1,0); CHECK(sf_recovery_feed(ctx,raw,188));
    pes_packet(raw,key,sizeof(key),1,1); CHECK(sf_recovery_feed(ctx,raw,188));
    CHECK(o.status.headers_found == 2 && o.status.progress_percent == 60);
    pes_packet(raw,key,sizeof(key),1,2); CHECK(sf_recovery_feed(ctx,raw,188));
    CHECK(sf_recovery_finish(ctx)); CHECK(o.status.progress_percent == 80); CHECK(o.candidates == 3);
    CHECK(o.status.keyframes == 2); sf_recovery_destroy(ctx);
    /* A partial PES can be tried without losing bytes needed by a later feed. */
    o.candidates = 0; o.features = 0; config.features = features;
    ctx = sf_recovery_create(&config); CHECK(ctx);
    pes_packet(raw,headers,sizeof(headers),1,0); CHECK(sf_recovery_feed(ctx,raw,188));
    pes_packet(raw,key,sizeof(key),1,1); CHECK(sf_recovery_feed(ctx,raw,188));
    CHECK(sf_recovery_analyze(ctx)); CHECK(o.candidates == 1);
    CHECK(o.status.keyframes == 1 && o.status.access_units == 1);
    CHECK(sf_recovery_analyze(ctx)); CHECK(o.candidates == 1);
    CHECK(sf_recovery_finish(ctx)); CHECK(o.status.keyframes == 1 && o.status.access_units == 2);
    CHECK(o.features == 2); sf_recovery_destroy(ctx);
    config.analyze_on_feed = 1; o.reject = 1;
    ctx = sf_recovery_create(&config); CHECK(ctx);
    pes_packet(raw,headers,sizeof(headers),1,0); CHECK(sf_recovery_feed(ctx,raw,188));
    pes_packet(raw,key,sizeof(key),1,1); CHECK(sf_recovery_feed(ctx,raw,188));
    CHECK(!sf_recovery_finish(ctx)); CHECK(!sf_recovery_feed(ctx,NULL,0));
    sf_recovery_destroy(ctx);
    config.video_pid = 8191; CHECK(!sf_recovery_create(&config));
}
static void test_oversized_and_noise(void) {
    SfRecoveryConfig c; sf_recovery_config_init(&c); c.video_pid = 256; c.max_bytes = 4096; c.decode = 0;
    SfRecovery *ctx = sf_recovery_create(&c); CHECK(ctx);
    uint8_t raw[188]; packet(raw,256,0);
    for (unsigned i = 0; i < 100; ++i) { raw[3] = (uint8_t)(0x10|(i&15)); CHECK(sf_recovery_feed(ctx,raw,188)); }
    CHECK(sf_recovery_finish(ctx)); SfRecoveryStatus s; CHECK(sf_recovery_get_status(ctx,&s));
    CHECK(s.oversized_units == 1 && s.retained_bytes <= c.max_bytes);
    uint32_t random = 1234567;
    for (int i = 0; i < 10000; ++i) {
        for (size_t j = 0; j < sizeof(raw); ++j) { random ^= random<<13; random ^= random>>17; random ^= random<<5; raw[j] = (uint8_t)random; }
        CHECK(sf_recovery_feed(ctx,raw,sizeof(raw)));
    }
    CHECK(sf_recovery_finish(ctx)); sf_recovery_destroy(ctx);
}
static void test_model_and_media(void) {
    /* Existing JSON model format, including reordered features. */
    const char *path = "/tmp/scatterfix-test-model.json";
    FILE *f = fopen(path,"w"); CHECK(f);
    fputs("{\"kind\":\"linear_regression_v1\",\"features\":[\"packets\",\"packet_confidence\"],\"weights\":[1,2,3],\"means\":{\"packets\":10},\"scales\":{\"packets\":2}}",f);
    CHECK(!fclose(f)); SfRankModel model; CHECK(sf_rank_model_load(&model,path));
    SfFeatures features = {0}; features.values[0] = .5; features.values[1] = 12;
    CHECK(fabs(sf_rank_score(&model,&features)-4.5) < 1e-9);
    f = fopen(path,"w"); CHECK(f); fputs("{\"weights\":[NaN]}",f); CHECK(!fclose(f));
    CHECK(!sf_rank_model_load(&model,path)); remove(path);
    uint8_t pixels[8*8*3];
    for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = (uint8_t)i;
    SfImage original = {0}; original.pixels = pixels; original.width = 8; original.height = 8; original.stride = 24;
    CHECK(sf_image_write_png("/tmp/scatterfix-test.png",&original));
    SfImage decoded = {0}; CHECK(sf_image_read("/tmp/scatterfix-test.png",&decoded));
    CHECK(decoded.width == 8 && decoded.height == 8);
    for (int y = 0; y < 8; ++y) CHECK(!memcmp(decoded.pixels+y*decoded.stride,pixels+y*24,24));
    sf_image_free(&decoded); remove("/tmp/scatterfix-test.png");
    CHECK(!sf_image_decode(NULL,0,SF_CODEC_H264,&decoded));
    CHECK(!sf_video_output_open("/tmp/unused.ts",3,3,1));
    SfVideoOutput *output = sf_video_output_open("/tmp/scatterfix-test.ts",8,8,1); CHECK(output);
    for (int i = 0; i < 3; ++i) CHECK(sf_video_output_send(output,&original));
    CHECK(sf_video_output_close(output)); CHECK(sf_image_read("/tmp/scatterfix-test.ts",&decoded));
    CHECK(decoded.width == 8 && decoded.height == 8); sf_image_free(&decoded); remove("/tmp/scatterfix-test.ts");
}
int main(void) {
    test_repair(); test_progress_and_voting(); test_oversized_and_noise(); test_model_and_media();
    test_capture("captures/on4vvv_2026-06-14_181433_70cm_rolling.ts",SF_CODEC_H264,"ON4VVV","Portsdown 4",1);
    test_capture("captures/dl5bca_2026-06-14_123600_rolling.ts",SF_CODEC_H265,"DL5BCA","0303",1316);
    puts("All C API tests passed."); return 0;
}
