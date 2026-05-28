#include "ts_packet.h"

int ts_packet_parse(const uint8_t pkt[TS_PACKET_SIZE], TsPacket *out) {
    if (!pkt || !out || pkt[0] != TS_SYNC_BYTE) {
        return 0;
    }

    out->bytes = pkt;
    out->transport_error = (uint8_t)((pkt[1] >> 7) & 1);
    out->payload_unit_start = (uint8_t)((pkt[1] >> 6) & 1);
    out->transport_priority = (uint8_t)((pkt[1] >> 5) & 1);
    out->pid = (uint16_t)(((pkt[1] & 0x1f) << 8) | pkt[2]);
    out->scrambling = (uint8_t)((pkt[3] >> 6) & 3);
    out->adaptation_field_control = (uint8_t)((pkt[3] >> 4) & 3);
    out->continuity_counter = (uint8_t)(pkt[3] & 0x0f);
    out->has_adaptation = (uint8_t)(out->adaptation_field_control == 2 || out->adaptation_field_control == 3);
    out->has_payload = (uint8_t)(out->adaptation_field_control == 1 || out->adaptation_field_control == 3);
    out->discontinuity_indicator = 0;
    out->has_pcr = 0;
    out->pcr_base = 0;
    out->payload_offset = 4;
    out->payload_size = 0;

    if (out->adaptation_field_control == 0) {
        return 0;
    }

    if (out->has_adaptation) {
        uint8_t len = pkt[4];
        if ((size_t)5 + len > TS_PACKET_SIZE) {
            return 0;
        }
        if (len > 0) {
            uint8_t flags = pkt[5];
            out->discontinuity_indicator = (uint8_t)((flags >> 7) & 1);
            out->has_pcr = (uint8_t)((flags >> 4) & 1);
            if (out->has_pcr && len >= 7) {
                const uint8_t *p = &pkt[6];
                out->pcr_base = ((uint64_t)p[0] << 25) | ((uint64_t)p[1] << 17) |
                                ((uint64_t)p[2] << 9) | ((uint64_t)p[3] << 1) |
                                ((uint64_t)p[4] >> 7);
            }
        }
        out->payload_offset = (size_t)5 + len;
    }

    if (out->has_payload && out->payload_offset < TS_PACKET_SIZE) {
        out->payload_size = TS_PACKET_SIZE - out->payload_offset;
    }

    return 1;
}

const char *ts_stream_type_name(uint8_t stream_type) {
    switch (stream_type) {
    case 0x02: return "MPEG-2 video";
    case 0x1b: return "H.264/AVC";
    case 0x24: return "H.265/HEVC";
    case 0x03: return "MPEG-1 audio";
    case 0x04: return "MPEG-2 audio";
    case 0x0f: return "AAC";
    case 0x06: return "private";
    default: return "unknown";
    }
}
