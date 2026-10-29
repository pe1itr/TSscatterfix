#ifndef SCATTERFIX_MEDIA_H
#define SCATTERFIX_MEDIA_H
#include "recovery.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Owned RGB storage. Free with sf_image_free. Returns 1 only for a decoded frame. */
int sf_image_decode(const uint8_t *data, size_t size, SfCodec codec, SfImage *image);
int sf_image_read(const char *path, SfImage *image);
int sf_image_write_png(const char *path, const SfImage *image);
void sf_image_free(SfImage *image);
/* Optional standalone output. The caller drives frame cadence by calling send.
 * Embedders displaying RGB pixels do not need to create this object. */
typedef struct SfVideoOutput SfVideoOutput;
SfVideoOutput *sf_video_output_open(const char *url, int width, int height, int fps);
int sf_video_output_send(SfVideoOutput *out, const SfImage *image);
int sf_video_output_close(SfVideoOutput *out);
#ifdef __cplusplus
}
#endif
#endif
