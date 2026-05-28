#include "ts_parser.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void ts_reader_init(TsReader *reader, TsInput *input, int verbose) {
    reader->input = input;
    reader->aligned = 0;
    reader->verbose = verbose;
}

static int read_byte(TsReader *reader) {
    uint8_t b;
    return ts_input_read(reader->input, &b, 1) == 1 ? b : EOF;
}

static size_t read_block(TsReader *reader, uint8_t *buf, size_t len) {
    return ts_input_read(reader->input, buf, len);
}

static int seek_sync(TsReader *reader, uint8_t packet[TS_PACKET_SIZE], Stats *stats) {
    int c;
    uint64_t dropped = 0;
    while ((c = read_byte(reader)) != EOF) {
        if ((uint8_t)c == TS_SYNC_BYTE) {
            packet[0] = TS_SYNC_BYTE;
            if (dropped > 0) {
                stats->bytes_dropped += dropped;
                stats->resync_count++;
                if (reader->verbose) {
                    fprintf(stderr, "[sync] dropped=%" PRIu64 " bytes before sync action=resync confidence=1.00\n", dropped);
                }
            }
            return 1;
        }
        dropped++;
    }
    if (dropped > 0) {
        stats->bytes_dropped += dropped;
        stats->packets_dropped++;
    }
    return 0;
}

int ts_reader_next(TsReader *reader, uint8_t packet[TS_PACKET_SIZE], Stats *stats) {
    for (;;) {
        if (!reader->aligned) {
            if (!seek_sync(reader, packet, stats)) {
                return 0;
            }
        } else {
            size_t got = read_block(reader, packet, TS_PACKET_SIZE);
            if (got == 0) {
                return 0;
            }
            if (got < TS_PACKET_SIZE) {
                stats->bytes_dropped += got;
                stats->packets_dropped++;
                if (reader->verbose) {
                    fprintf(stderr, "[sync] trailing partial packet bytes=%zu action=drop confidence=1.00\n", got);
                }
                return 0;
            }
            if (packet[0] == TS_SYNC_BYTE) {
                stats->packets_read++;
                return 1;
            }
            stats->packets_dropped++;
            stats->resync_count++;
            stats->bytes_dropped++;
            if (reader->verbose) {
                fprintf(stderr, "[sync] lost alignment at packet=%" PRIu64 " action=resync confidence=1.00\n",
                        stats->packets_read + 1);
            }
            reader->aligned = 0;
            for (size_t i = 1; i < TS_PACKET_SIZE; i++) {
                if (packet[i] == TS_SYNC_BYTE) {
                    memmove(packet, &packet[i], TS_PACKET_SIZE - i);
                    size_t need = i;
                    size_t got = read_block(reader, packet + (TS_PACKET_SIZE - i), need);
                    stats->bytes_dropped += i;
                    if (got == need) {
                        reader->aligned = 1;
                        stats->packets_read++;
                        return 1;
                    }
                    stats->bytes_dropped += got;
                    return 0;
                }
            }
            continue;
        }

        size_t got = read_block(reader, packet + 1, TS_PACKET_SIZE - 1);
        if (got < TS_PACKET_SIZE - 1) {
            stats->bytes_dropped += got + 1;
            stats->packets_dropped++;
            return 0;
        }
        reader->aligned = 1;
        stats->packets_read++;
        return 1;
    }
}
