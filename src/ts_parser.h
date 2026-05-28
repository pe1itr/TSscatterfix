#ifndef TS_PARSER_H
#define TS_PARSER_H

#include <stdint.h>
#include <stdio.h>

#include "io.h"
#include "stats.h"
#include "ts_packet.h"

typedef struct {
    TsInput *input;
    int aligned;
    int verbose;
} TsReader;

void ts_reader_init(TsReader *reader, TsInput *input, int verbose);
int ts_reader_next(TsReader *reader, uint8_t packet[TS_PACKET_SIZE], Stats *stats);

#endif
