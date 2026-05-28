#ifndef ML_MODEL_H
#define ML_MODEL_H

#include <stdint.h>
#include <stdio.h>

#include "psi.h"
#include "stats.h"
#include "ts_packet.h"

#define ML_REPEAT_HISTORY_MAX 64
#define ML_REPEAT_HISTORY_NORMAL 16
#define ML_REPEAT_HISTORY_CONTEST 64

typedef enum {
    PID_ROLE_PAT = 0,
    PID_ROLE_PMT,
    PID_ROLE_VIDEO,
    PID_ROLE_AUDIO,
    PID_ROLE_PCR,
    PID_ROLE_NULL,
    PID_ROLE_UNKNOWN,
    PID_ROLE_COUNT
} PidRole;

typedef struct {
    double role[PID_ROLE_COUNT];
    uint64_t packets;
    uint64_t pes_starts;
    uint64_t pcr_packets;
    uint64_t repeated_packets;
    uint64_t repeated_payloads;
    uint64_t last_pes_packet;
    uint64_t last_pcr_packet;
    double pes_interval;
    double pcr_interval;
    uint32_t packet_hashes[ML_REPEAT_HISTORY_MAX];
    uint32_t payload_hashes[ML_REPEAT_HISTORY_MAX];
    uint8_t packet_hash_pos;
    uint8_t payload_hash_pos;
} PidModel;

typedef struct {
    PidModel pid[TS_PID_COUNT];
    int enabled;
    int contest_mode;
    uint8_t repeat_window;
    char state_path[512];
} MlModel;

void ml_model_init(MlModel *model, int enabled, int contest_mode, const char *state_path);
void ml_model_observe(MlModel *model, const TsPacket *pkt, const PsiContext *psi, Stats *stats,
                      uint64_t packet_index, int verbose);
void ml_model_print_roles(FILE *err, const MlModel *model, const PsiContext *psi);
int ml_model_load(MlModel *model);
int ml_model_save(const MlModel *model);
const char *ml_role_name(PidRole role);

#endif
