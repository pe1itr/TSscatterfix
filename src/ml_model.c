#include "ml_model.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void add_role(PidModel *p, PidRole role, double amount) {
    p->role[role] += amount;
    if (p->role[role] > 1.0) {
        p->role[role] = 1.0;
    }
    for (int i = 0; i < PID_ROLE_COUNT; i++) {
        if (i != (int)role) {
            p->role[i] *= 0.9995;
        }
    }
}

const char *ml_role_name(PidRole role) {
    switch (role) {
    case PID_ROLE_PAT: return "PAT";
    case PID_ROLE_PMT: return "PMT";
    case PID_ROLE_VIDEO: return "VIDEO";
    case PID_ROLE_AUDIO: return "AUDIO";
    case PID_ROLE_PCR: return "PCR";
    case PID_ROLE_NULL: return "NULL";
    case PID_ROLE_UNKNOWN: return "UNKNOWN";
    default: return "?";
    }
}

void ml_model_init(MlModel *model, int enabled, int contest_mode, const char *state_path) {
    memset(model, 0, sizeof(*model));
    model->enabled = enabled;
    model->contest_mode = contest_mode;
    model->repeat_window = contest_mode ? ML_REPEAT_HISTORY_CONTEST : ML_REPEAT_HISTORY_NORMAL;
    for (int pid = 0; pid < TS_PID_COUNT; pid++) {
        model->pid[pid].role[PID_ROLE_UNKNOWN] = 0.1;
    }
    if (state_path) {
        snprintf(model->state_path, sizeof(model->state_path), "%s", state_path);
    }
}

static int pid_in_list(uint16_t pid, const uint16_t *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (list[i] == pid) {
            return 1;
        }
    }
    return 0;
}

static uint32_t fnv1a_update(uint32_t hash, uint8_t byte) {
    hash ^= byte;
    return hash * 16777619u;
}

static uint32_t packet_hash_ignore_cc(const TsPacket *pkt) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < TS_PACKET_SIZE; i++) {
        uint8_t b = pkt->bytes[i];
        if (i == 3) {
            b &= 0xf0;
        }
        h = fnv1a_update(h, b);
    }
    return h;
}

static uint32_t payload_hash(const TsPacket *pkt) {
    uint32_t h = 2166136261u;
    for (size_t i = pkt->payload_offset; i < TS_PACKET_SIZE; i++) {
        h = fnv1a_update(h, pkt->bytes[i]);
    }
    return h;
}

static int remember_hash(uint32_t hashes[ML_REPEAT_HISTORY_MAX], uint8_t *pos, uint8_t window, uint32_t hash) {
    int repeated = 0;
    if (hash != 0) {
        for (int i = 0; i < window; i++) {
            if (hashes[i] == hash) {
                repeated = 1;
                break;
            }
        }
    }
    hashes[*pos] = hash ? hash : 1u;
    *pos = (uint8_t)((*pos + 1u) % window);
    return repeated;
}

void ml_model_observe(MlModel *model, const TsPacket *pkt, const PsiContext *psi, Stats *stats,
                      uint64_t packet_index, int verbose) {
    if (!model->enabled || pkt->pid >= TS_PID_COUNT) {
        return;
    }

    PidModel *p = &model->pid[pkt->pid];
    p->packets++;

    if (remember_hash(p->packet_hashes, &p->packet_hash_pos, model->repeat_window, packet_hash_ignore_cc(pkt))) {
        p->repeated_packets++;
        stats->repeated_packet_candidates++;
    }
    if (pkt->has_payload && pkt->payload_size >= 16 &&
        remember_hash(p->payload_hashes, &p->payload_hash_pos, model->repeat_window, payload_hash(pkt))) {
        p->repeated_payloads++;
        stats->repeated_payload_candidates++;
    }

    if (pkt->pid == 0x0000) {
        add_role(p, PID_ROLE_PAT, 0.08);
    } else if (pkt->pid == TS_NULL_PID) {
        add_role(p, PID_ROLE_NULL, 0.08);
    } else if (psi_pid_is_pmt(psi, pkt->pid)) {
        add_role(p, PID_ROLE_PMT, 0.08);
    }
    if (pid_in_list(pkt->pid, psi->video_pids, psi->video_count)) {
        add_role(p, PID_ROLE_VIDEO, 0.06);
    }
    if (pid_in_list(pkt->pid, psi->audio_pids, psi->audio_count)) {
        add_role(p, PID_ROLE_AUDIO, 0.06);
    }
    if (pkt->has_pcr || pkt->pid == psi->pcr_pid) {
        add_role(p, PID_ROLE_PCR, 0.04);
    }

    if (pkt->payload_unit_start && pkt->has_payload && pkt->payload_size >= 3 &&
        pkt->bytes[pkt->payload_offset] == 0x00 && pkt->bytes[pkt->payload_offset + 1] == 0x00 &&
        pkt->bytes[pkt->payload_offset + 2] == 0x01) {
        if (p->last_pes_packet != 0) {
            uint64_t gap = packet_index - p->last_pes_packet;
            p->pes_interval = p->pes_interval == 0.0 ? (double)gap : (p->pes_interval * 0.90 + (double)gap * 0.10);
            if (verbose && p->pes_interval > 0.0 && gap > (uint64_t)(p->pes_interval * 4.0 + 20.0)) {
                fprintf(stderr, "[ml] pid=0x%04x large PES gap=%" PRIu64 " typical=%.1f confidence=0.65\n",
                        pkt->pid, gap, p->pes_interval);
            }
        }
        p->last_pes_packet = packet_index;
        p->pes_starts++;
    }

    if (pkt->has_pcr) {
        if (p->last_pcr_packet != 0) {
            uint64_t gap = packet_index - p->last_pcr_packet;
            p->pcr_interval = p->pcr_interval == 0.0 ? (double)gap : (p->pcr_interval * 0.90 + (double)gap * 0.10);
            if (verbose) {
                fprintf(stderr, "[ml] pid=0x%04x PCR interval=%" PRIu64 " typical=%.1f confidence=0.70\n",
                        pkt->pid, gap, p->pcr_interval);
            }
        }
        p->last_pcr_packet = packet_index;
        p->pcr_packets++;
    }
}

void ml_model_print_roles(FILE *err, const MlModel *model, const PsiContext *psi) {
    if (!model->enabled) {
        return;
    }
    if (model->contest_mode) {
        fprintf(err, "[contest] mode=enabled timing=async_recovered_view repeat_window=%u media_payload_repair=planned\n", model->repeat_window);
    }
    uint16_t interesting[8 + PSI_MAX_STREAMS * 2];
    size_t n = 0;
    interesting[n++] = 0x0000;
    if (psi->primary_pmt_pid != TS_NULL_PID) interesting[n++] = psi->primary_pmt_pid;
    if (psi->pcr_pid != TS_NULL_PID) interesting[n++] = psi->pcr_pid;
    for (size_t i = 0; i < psi->video_count && n < sizeof(interesting) / sizeof(interesting[0]); i++) interesting[n++] = psi->video_pids[i];
    for (size_t i = 0; i < psi->audio_count && n < sizeof(interesting) / sizeof(interesting[0]); i++) interesting[n++] = psi->audio_pids[i];

    for (size_t i = 0; i < n; i++) {
        const PidModel *p = &model->pid[interesting[i]];
        fprintf(err, "[ml] pid=0x%04x roles PAT=%.2f PMT=%.2f VIDEO=%.2f AUDIO=%.2f PCR=%.2f NULL=%.2f repeat_pkt=%" PRIu64 " repeat_payload=%" PRIu64 "\n",
                interesting[i], p->role[PID_ROLE_PAT], p->role[PID_ROLE_PMT], p->role[PID_ROLE_VIDEO],
                p->role[PID_ROLE_AUDIO], p->role[PID_ROLE_PCR], p->role[PID_ROLE_NULL],
                p->repeated_packets, p->repeated_payloads);
    }
}

int ml_model_load(MlModel *model) {
    if (!model->enabled || model->state_path[0] == '\0') {
        return 0;
    }
    FILE *f = fopen(model->state_path, "r");
    if (!f) {
        return 0;
    }
    unsigned pid;
    double pat, pmt, video, audio, pcr, null_role;
    while (fscanf(f, "pid=%x pat=%lf pmt=%lf video=%lf audio=%lf pcr=%lf null=%lf\n",
                  &pid, &pat, &pmt, &video, &audio, &pcr, &null_role) == 7) {
        if (pid < TS_PID_COUNT) {
            model->pid[pid].role[PID_ROLE_PAT] = pat;
            model->pid[pid].role[PID_ROLE_PMT] = pmt;
            model->pid[pid].role[PID_ROLE_VIDEO] = video;
            model->pid[pid].role[PID_ROLE_AUDIO] = audio;
            model->pid[pid].role[PID_ROLE_PCR] = pcr;
            model->pid[pid].role[PID_ROLE_NULL] = null_role;
        }
    }
    fclose(f);
    return 1;
}

int ml_model_save(const MlModel *model) {
    if (!model->enabled || model->state_path[0] == '\0') {
        return 0;
    }
    FILE *f = fopen(model->state_path, "w");
    if (!f) {
        return -1;
    }
    for (unsigned pid = 0; pid < TS_PID_COUNT; pid++) {
        const PidModel *p = &model->pid[pid];
        if (p->packets || p->role[PID_ROLE_PAT] > 0.1 || p->role[PID_ROLE_PMT] > 0.1 ||
            p->role[PID_ROLE_VIDEO] > 0.1 || p->role[PID_ROLE_AUDIO] > 0.1 || p->role[PID_ROLE_PCR] > 0.1) {
            fprintf(f, "pid=%04x pat=%.6f pmt=%.6f video=%.6f audio=%.6f pcr=%.6f null=%.6f\n",
                    pid, p->role[PID_ROLE_PAT], p->role[PID_ROLE_PMT], p->role[PID_ROLE_VIDEO],
                    p->role[PID_ROLE_AUDIO], p->role[PID_ROLE_PCR], p->role[PID_ROLE_NULL]);
        }
    }
    fclose(f);
    return 1;
}
