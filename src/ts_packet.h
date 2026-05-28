#ifndef TS_PACKET_H
#define TS_PACKET_H

#include <stdint.h>
#include <stddef.h>

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47
#define TS_PID_COUNT 8192
#define TS_NULL_PID 0x1fff

typedef struct {
    const uint8_t *bytes;
    uint16_t pid;
    uint8_t transport_error;
    uint8_t payload_unit_start;
    uint8_t transport_priority;
    uint8_t scrambling;
    uint8_t adaptation_field_control;
    uint8_t continuity_counter;
    uint8_t has_adaptation;
    uint8_t has_payload;
    uint8_t discontinuity_indicator;
    uint8_t has_pcr;
    uint64_t pcr_base;
    size_t payload_offset;
    size_t payload_size;
} TsPacket;

int ts_packet_parse(const uint8_t pkt[TS_PACKET_SIZE], TsPacket *out);
const char *ts_stream_type_name(uint8_t stream_type);

#endif
