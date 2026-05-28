#include "contest.h"

#include <inttypes.h>
#include <string.h>

static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static int pid_in_list(uint16_t pid, const uint16_t *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (list[i] == pid) {
            return 1;
        }
    }
    return 0;
}

void contest_init(ContestContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void contest_observe_packet(ContestContext *ctx, const TsPacket *pkt, const PsiContext *psi, uint64_t packet_index) {
    if (!pkt->has_payload || pkt->payload_size == 0 || pkt->payload_offset >= TS_PACKET_SIZE) {
        return;
    }

    size_t payload_size = pkt->payload_size;
    if (payload_size > CONTEST_FRAGMENT_PAYLOAD_MAX) {
        payload_size = CONTEST_FRAGMENT_PAYLOAD_MAX;
    }

    ContestFragment *frag = &ctx->fragments[ctx->next_slot];
    if (ctx->fragments_stored >= CONTEST_FRAGMENT_CAPACITY) {
        ctx->bytes_stored -= frag->payload_size;
    } else {
        ctx->fragments_stored++;
    }

    memset(frag, 0, sizeof(*frag));
    frag->packet_index = packet_index;
    frag->pid = pkt->pid;
    frag->continuity_counter = pkt->continuity_counter;
    frag->payload_unit_start = pkt->payload_unit_start;
    frag->transport_error = pkt->transport_error;
    frag->scrambling = pkt->scrambling;
    frag->discontinuity_indicator = pkt->discontinuity_indicator;
    frag->payload_size = (uint8_t)payload_size;
    memcpy(frag->payload, pkt->bytes + pkt->payload_offset, payload_size);
    frag->payload_hash = fnv1a(frag->payload, payload_size);
    frag->confidence = 1.0;
    if (frag->transport_error) {
        frag->confidence *= 0.20;
    }
    if (frag->scrambling) {
        frag->confidence *= 0.35;
    }
    if (frag->discontinuity_indicator) {
        frag->confidence *= 0.70;
    }

    ctx->fragments_seen++;
    ctx->bytes_stored += payload_size;
    ctx->pid_fragment_count[pkt->pid]++;
    if (frag->transport_error) ctx->tei_fragments++;
    if (frag->payload_unit_start) ctx->pusi_fragments++;
    if (frag->scrambling) ctx->scrambled_fragments++;
    if (frag->discontinuity_indicator) ctx->discontinuity_fragments++;
    if (psi_pid_is_known_si(psi, pkt->pid)) ctx->psi_fragments++;
    if (pid_in_list(pkt->pid, psi->video_pids, psi->video_count)) ctx->video_fragments++;
    if (pid_in_list(pkt->pid, psi->audio_pids, psi->audio_count)) ctx->audio_fragments++;

    ctx->next_slot = (ctx->next_slot + 1u) % CONTEST_FRAGMENT_CAPACITY;
}

static void print_pid_array(FILE *out, const char *name, const uint16_t *pids, size_t count) {
    fprintf(out, "\"%s\":[", name);
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "%s%u", i ? "," : "", pids[i]);
    }
    fprintf(out, "]");
}

static void print_hot_pids(FILE *out, const ContestContext *contest) {
    uint16_t top_pid[8] = {0};
    uint64_t top_count[8] = {0};

    for (uint16_t pid = 0; pid < TS_PID_COUNT; pid++) {
        uint64_t count = contest->pid_fragment_count[pid];
        if (count == 0 || count <= top_count[7]) {
            continue;
        }
        size_t pos = 7;
        while (pos > 0 && count > top_count[pos - 1]) {
            top_count[pos] = top_count[pos - 1];
            top_pid[pos] = top_pid[pos - 1];
            pos--;
        }
        top_count[pos] = count;
        top_pid[pos] = pid;
    }

    fprintf(out, "\"top_pids\":[");
    for (size_t i = 0; i < 8 && top_count[i] > 0; i++) {
        fprintf(out, "%s{\"pid\":%u,\"fragments\":%" PRIu64 "}", i ? "," : "", top_pid[i], top_count[i]);
    }
    fprintf(out, "]");
}

void contest_print_json(FILE *out, const ContestContext *contest, const Stats *stats,
                        const PsiContext *psi, uint64_t interval_index) {
    fprintf(out,
            "{\"type\":\"tsscatterfix_status\",\"interval\":%" PRIu64
            ",\"packets\":{\"read\":%" PRIu64 ",\"written\":%" PRIu64 ",\"dropped\":%" PRIu64
            ",\"resync\":%" PRIu64 ",\"cc_errors\":%" PRIu64 ",\"missing\":%" PRIu64
            ",\"duplicates\":%" PRIu64 ",\"out_of_order\":%" PRIu64 "},",
            interval_index, stats->packets_read, stats->packets_written, stats->packets_dropped,
            stats->resync_count, stats->continuity_errors, stats->missing_packets,
            stats->duplicates, stats->out_of_order);

    fprintf(out,
            "\"repair\":{\"actions\":%" PRIu64 ",\"psi_injections\":%" PRIu64
            ",\"tei\":%" PRIu64 ",\"invalid_ts\":%" PRIu64 "},",
            stats->repair_actions, stats->psi_injections,
            stats->transport_error_packets, stats->invalid_packets);

    fprintf(out,
            "\"psi\":{\"pat\":%s,\"pmt\":%s,\"sdt\":%s,\"pmt_pid\":%u,\"pcr_pid\":%u,",
            psi->has_pat ? "true" : "false", psi->has_pmt ? "true" : "false",
            psi->has_sdt ? "true" : "false", psi->primary_pmt_pid, psi->pcr_pid);
    print_pid_array(out, "video_pids", psi->video_pids, psi->video_count);
    fprintf(out, ",");
    print_pid_array(out, "audio_pids", psi->audio_pids, psi->audio_count);
    fprintf(out, "},");

    fprintf(out,
            "\"contest\":{\"fragment_capacity\":%u,\"fragments_seen\":%" PRIu64
            ",\"fragments_stored\":%" PRIu64 ",\"bytes_stored\":%" PRIu64
            ",\"pusi_fragments\":%" PRIu64 ",\"tei_fragments\":%" PRIu64
            ",\"scrambled_fragments\":%" PRIu64 ",\"discontinuity_fragments\":%" PRIu64
            ",\"psi_fragments\":%" PRIu64 ",\"video_fragments\":%" PRIu64
            ",\"audio_fragments\":%" PRIu64 ",",
            CONTEST_FRAGMENT_CAPACITY, contest->fragments_seen, contest->fragments_stored,
            contest->bytes_stored, contest->pusi_fragments, contest->tei_fragments,
            contest->scrambled_fragments, contest->discontinuity_fragments,
            contest->psi_fragments, contest->video_fragments, contest->audio_fragments);
    print_hot_pids(out, contest);
    fprintf(out, "}}\n");
}
