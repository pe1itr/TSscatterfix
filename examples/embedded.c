/* Build: make examples/embedded
 * This headless example is also the integration boundary for any GUI toolkit.
 * Run: examples/embedded capture.ts
 */
#include "recovery.h"
#include <stdio.h>

static void show_image(void *app, const SfImage *image) {
    (void)app;
    /* Copy image->pixels to your application's image/texture, using stride.
     * If feed runs on a worker thread, queue the COPY to the GUI thread.
     * The callback's pixel pointer must not be retained after returning. */
    printf("Display RGB24: %dx%d, stride %d, version %llu\n",
           image->width, image->height, image->stride,
           (unsigned long long)image->version);
}
static void show_status(void *app, const SfRecoveryStatus *status) {
    (void)app;
    printf("%s / %s: %u%% (estimate), headers %u/%u, keyframes %llu\n",
           status->service_name, status->provider_name,
           status->progress_percent, status->headers_found, status->headers_required,
           (unsigned long long)status->keyframes);
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s capture.ts\n", argv[0]); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror(argv[1]); return 1; }
    SfRecoveryConfig config;
    sf_recovery_config_init(&config);
    config.image = show_image;
    config.status = show_status;
    config.userdata = NULL; /* Replace with your application/window state. */
    SfRecovery *recovery = sf_recovery_create(&config);
    if (!recovery) { fclose(file); return 1; }
    uint8_t data[1316]; size_t n; int ok = 1;
    while ((n = fread(data, 1, sizeof(data), file)) != 0)
        if (!sf_recovery_feed(recovery, data, n)) { ok = 0; break; }
    if (ferror(file) || !sf_recovery_finish(recovery)) ok = 0;
    sf_recovery_destroy(recovery);
    if (fclose(file)) ok = 0;
    return ok ? 0 : 1;
}
