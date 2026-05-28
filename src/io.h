#ifndef IO_H
#define IO_H

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ts_packet.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET TsSocket;
#else
#include <sys/socket.h>
typedef int TsSocket;
#endif

typedef enum {
    TS_IO_FILE = 0,
    TS_IO_UDP
} TsIoKind;

typedef struct {
    TsIoKind kind;
    FILE *file;
    TsSocket sock;
    uint8_t udp_buf[65536];
    size_t udp_pos;
    size_t udp_len;
} TsInput;

typedef struct {
    TsIoKind kind;
    FILE *file;
    TsSocket sock;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint8_t udp_out_buf[TS_PACKET_SIZE * 7];
    size_t udp_out_packets;
    unsigned udp_rate_kbit;
} TsOutput;

int ts_io_startup(void);
void ts_io_cleanup(void);

void ts_input_init_file(TsInput *input, FILE *file);
int ts_input_init_udp(TsInput *input, const char *port);
size_t ts_input_read(TsInput *input, uint8_t *buf, size_t len);
void ts_input_close(TsInput *input);

void ts_output_init_file(TsOutput *output, FILE *file);
int ts_output_init_udp(TsOutput *output, const char *host_port);
void ts_output_set_udp_rate(TsOutput *output, unsigned kbit_per_sec);
int ts_output_write_packet(TsOutput *output, const uint8_t packet[TS_PACKET_SIZE]);
int ts_output_flush(TsOutput *output);
void ts_output_close(TsOutput *output);

#endif
