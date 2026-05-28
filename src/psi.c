#include "psi.h"

#include <stdio.h>
#include <string.h>

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t psi_mpeg_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04c11db7u) : (crc << 1);
        }
    }
    return crc;
}

void psi_init(PsiContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->primary_pmt_pid = TS_NULL_PID;
    ctx->service_id = TS_NULL_PID;
    ctx->pcr_pid = TS_NULL_PID;
}

static double candidate_confidence(const PsiCandidate *c, uint64_t now) {
    if (!c->valid) {
        return 0.0;
    }
    uint64_t age = now > c->last_seen_packet ? now - c->last_seen_packet : 0;
    double recency = 1.0 / (1.0 + (double)age / 50000.0);
    return c->weight * recency;
}

static void remember_candidate(PsiCandidate list[PSI_MAX_CANDIDATES], uint16_t pid, const uint8_t pkt[TS_PACKET_SIZE],
                               uint8_t version, uint32_t crc, double score, uint64_t packet_index) {
    int slot = -1;
    int weakest = 0;
    double weakest_score = 1e30;

    for (int i = 0; i < PSI_MAX_CANDIDATES; i++) {
        if (list[i].valid && list[i].pid == pid && list[i].version == version && list[i].crc == crc) {
            slot = i;
            break;
        }
        double s = list[i].valid ? list[i].weight : -1.0;
        if (!list[i].valid || s < weakest_score) {
            weakest = i;
            weakest_score = s;
        }
    }

    if (slot < 0) {
        slot = weakest;
        memset(&list[slot], 0, sizeof(list[slot]));
        list[slot].valid = 1;
        list[slot].pid = pid;
        list[slot].version = version;
        list[slot].crc = crc;
        list[slot].weight = score;
    } else {
        list[slot].weight = list[slot].weight * 0.85 + score;
    }

    memcpy(list[slot].data, pkt, TS_PACKET_SIZE);
    list[slot].last_seen_packet = packet_index;
    list[slot].seen_count++;
}

static double best_confidence(PsiCandidate list[PSI_MAX_CANDIDATES], uint64_t packet_index) {
    double best = 0.0;
    for (int i = 0; i < PSI_MAX_CANDIDATES; i++) {
        double c = candidate_confidence(&list[i], packet_index);
        if (c > best) {
            best = c;
        }
    }
    return best > 1.0 ? 1.0 : best;
}

static const uint8_t *section_from_payload(const TsPacket *pkt, size_t *section_len) {
    if (!pkt->has_payload || pkt->payload_size < 1) {
        return NULL;
    }
    size_t off = pkt->payload_offset;
    if (pkt->payload_unit_start) {
        uint8_t pointer = pkt->bytes[off];
        off++;
        if (off + pointer >= TS_PACKET_SIZE) {
            return NULL;
        }
        off += pointer;
    }
    if (off + 3 > TS_PACKET_SIZE) {
        return NULL;
    }
    const uint8_t *s = &pkt->bytes[off];
    size_t len = (size_t)(((s[1] & 0x0f) << 8) | s[2]) + 3;
    if (len < 8 || off + len > TS_PACKET_SIZE) {
        return NULL;
    }
    *section_len = len;
    return s;
}

static int parse_pat(PsiContext *ctx, const TsPacket *pkt, const uint8_t *s, size_t len, uint64_t packet_index, int verbose) {
    if (s[0] != 0x00 || len < 12 || psi_mpeg_crc32(s, len) != 0) {
        return 0;
    }

    uint8_t version = (uint8_t)((s[5] >> 1) & 0x1f);
    uint16_t best_pmt = TS_NULL_PID;
    uint16_t best_service = TS_NULL_PID;
    for (size_t pos = 8; pos + 4 <= len - 4; pos += 4) {
        uint16_t program = read16(&s[pos]);
        uint16_t pid = (uint16_t)(((s[pos + 2] & 0x1f) << 8) | s[pos + 3]);
        if (program != 0) {
            best_service = program;
            best_pmt = pid;
            break;
        }
    }
    if (best_pmt == TS_NULL_PID) {
        return 0;
    }

    ctx->has_pat = 1;
    ctx->primary_pmt_pid = best_pmt;
    ctx->service_id = best_service;
    memcpy(ctx->cached_pat, pkt->bytes, TS_PACKET_SIZE);
    ctx->has_cached_pat = 1;
    remember_candidate(ctx->pat_candidates, 0, pkt->bytes, version, psi_mpeg_crc32(s, len - 4), 0.35, packet_index);
    ctx->best_pat_confidence = best_confidence(ctx->pat_candidates, packet_index);

    if (verbose) {
        fprintf(stderr, "[psi] PAT version=%u pmt_pid=0x%04x confidence=%.3f\n",
                version, ctx->primary_pmt_pid, ctx->best_pat_confidence);
    }
    return 1;
}

static void copy_dvb_text(char *dst, size_t dst_size, const uint8_t *src, size_t len) {
    size_t out = 0;
    if (dst_size == 0) {
        return;
    }
    if (len > 0 && (src[0] == 0x05 || src[0] == 0x10 || src[0] == 0x11 || src[0] == 0x12 || src[0] == 0x15)) {
        src++;
        len--;
    }
    for (size_t i = 0; i < len && out + 1 < dst_size; i++) {
        uint8_t c = src[i];
        dst[out++] = (char)((c >= 0x20 && c <= 0x7e) ? c : '?');
    }
    dst[out] = '\0';
}

static int parse_sdt(PsiContext *ctx, const TsPacket *pkt, const uint8_t *s, size_t len, uint64_t packet_index, int verbose) {
    if ((s[0] != 0x42 && s[0] != 0x46) || len < 15 || psi_mpeg_crc32(s, len) != 0) {
        return 0;
    }

    uint8_t version = (uint8_t)((s[5] >> 1) & 0x1f);
    size_t pos = 11;
    int found_name = 0;

    while (pos + 5 <= len - 4) {
        uint16_t service_id = read16(&s[pos]);
        uint16_t descriptors_len = (uint16_t)(((s[pos + 3] & 0x0f) << 8) | s[pos + 4]);
        size_t dpos = pos + 5;
        size_t dend = dpos + descriptors_len;
        if (dend > len - 4) {
            return 0;
        }

        while (dpos + 2 <= dend) {
            uint8_t tag = s[dpos];
            uint8_t dlen = s[dpos + 1];
            const uint8_t *d = &s[dpos + 2];
            if (dpos + 2u + dlen > dend) {
                return 0;
            }
            if (tag == 0x48 && dlen >= 3) {
                uint8_t provider_len = d[1];
                if ((size_t)2 + provider_len + 1 <= dlen) {
                    uint8_t service_len = d[2 + provider_len];
                    if ((size_t)3 + provider_len + service_len <= dlen &&
                        (ctx->service_id == TS_NULL_PID || ctx->service_id == service_id || !found_name)) {
                        ctx->service_id = service_id;
                        copy_dvb_text(ctx->provider_name, sizeof(ctx->provider_name), &d[2], provider_len);
                        copy_dvb_text(ctx->service_name, sizeof(ctx->service_name), &d[3 + provider_len], service_len);
                        found_name = 1;
                    }
                }
            }
            dpos += 2u + dlen;
        }
        pos = dend;
    }

    ctx->has_sdt = 1;
    memcpy(ctx->cached_sdt, pkt->bytes, TS_PACKET_SIZE);
    ctx->has_cached_sdt = 1;
    remember_candidate(ctx->sdt_candidates, pkt->pid, pkt->bytes, version, psi_mpeg_crc32(s, len - 4), 0.30, packet_index);
    ctx->best_sdt_confidence = best_confidence(ctx->sdt_candidates, packet_index);

    if (verbose) {
        fprintf(stderr, "[psi] SDT version=%u service_id=0x%04x provider=\"%s\" service=\"%s\" confidence=%.3f\n",
                version, ctx->service_id, ctx->provider_name, ctx->service_name, ctx->best_sdt_confidence);
    }
    return 1;
}

static int is_video_type(uint8_t t) {
    return t == 0x02 || t == 0x1b || t == 0x24;
}

static int is_audio_type(uint8_t t) {
    return t == 0x03 || t == 0x04 || t == 0x0f;
}

static int parse_pmt(PsiContext *ctx, const TsPacket *pkt, const uint8_t *s, size_t len, uint64_t packet_index, int verbose) {
    if (s[0] != 0x02 || len < 16 || psi_mpeg_crc32(s, len) != 0) {
        return 0;
    }

    uint8_t version = (uint8_t)((s[5] >> 1) & 0x1f);
    uint16_t pcr_pid = (uint16_t)(((s[8] & 0x1f) << 8) | s[9]);
    uint16_t program_info_len = (uint16_t)(((s[10] & 0x0f) << 8) | s[11]);
    size_t pos = 12u + program_info_len;
    if (pos > len - 4) {
        return 0;
    }

    ctx->video_count = 0;
    ctx->audio_count = 0;
    ctx->pcr_pid = pcr_pid;
    while (pos + 5 <= len - 4) {
        uint8_t stream_type = s[pos];
        uint16_t elem_pid = (uint16_t)(((s[pos + 1] & 0x1f) << 8) | s[pos + 2]);
        uint16_t es_info_len = (uint16_t)(((s[pos + 3] & 0x0f) << 8) | s[pos + 4]);
        if (is_video_type(stream_type) && ctx->video_count < PSI_MAX_STREAMS) {
            ctx->video_types[ctx->video_count] = stream_type;
            ctx->video_pids[ctx->video_count++] = elem_pid;
        } else if (is_audio_type(stream_type) && ctx->audio_count < PSI_MAX_STREAMS) {
            ctx->audio_types[ctx->audio_count] = stream_type;
            ctx->audio_pids[ctx->audio_count++] = elem_pid;
        }
        pos += 5u + es_info_len;
    }

    ctx->has_pmt = 1;
    memcpy(ctx->cached_pmt, pkt->bytes, TS_PACKET_SIZE);
    ctx->has_cached_pmt = 1;
    remember_candidate(ctx->pmt_candidates, pkt->pid, pkt->bytes, version, psi_mpeg_crc32(s, len - 4), 0.35, packet_index);
    ctx->best_pmt_confidence = best_confidence(ctx->pmt_candidates, packet_index);

    if (verbose) {
        fprintf(stderr, "[psi] PMT pid=0x%04x version=%u pcr_pid=0x%04x video=%zu audio=%zu confidence=%.3f\n",
                pkt->pid, version, ctx->pcr_pid, ctx->video_count, ctx->audio_count, ctx->best_pmt_confidence);
    }
    return 1;
}

void psi_observe_packet(PsiContext *ctx, const TsPacket *pkt, uint64_t packet_index, int verbose) {
    size_t len = 0;
    const uint8_t *s = section_from_payload(pkt, &len);
    if (!s) {
        return;
    }
    if (pkt->pid == 0x0000) {
        (void)parse_pat(ctx, pkt, s, len, packet_index, verbose);
    } else if (pkt->pid == 0x0011) {
        (void)parse_sdt(ctx, pkt, s, len, packet_index, verbose);
    } else if (psi_pid_is_pmt(ctx, pkt->pid)) {
        (void)parse_pmt(ctx, pkt, s, len, packet_index, verbose);
    }
}

uint8_t psi_pid_is_pmt(const PsiContext *ctx, uint16_t pid) {
    return (uint8_t)(ctx->primary_pmt_pid != TS_NULL_PID && pid == ctx->primary_pmt_pid);
}

uint8_t psi_pid_is_known_si(const PsiContext *ctx, uint16_t pid) {
    return (uint8_t)(pid == 0x0000 || pid == 0x0011 || psi_pid_is_pmt(ctx, pid));
}

uint8_t psi_packet_has_valid_section(const PsiContext *ctx, const TsPacket *pkt) {
    size_t len = 0;
    const uint8_t *s = section_from_payload(pkt, &len);
    if (!s || psi_mpeg_crc32(s, len) != 0) {
        return 0;
    }
    if (pkt->pid == 0x0000) {
        return (uint8_t)(s[0] == 0x00 && len >= 12);
    }
    if (pkt->pid == 0x0011) {
        return (uint8_t)((s[0] == 0x42 || s[0] == 0x46) && len >= 15);
    }
    if (psi_pid_is_pmt(ctx, pkt->pid)) {
        return (uint8_t)(s[0] == 0x02 && len >= 16);
    }
    return 0;
}
