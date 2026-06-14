#include "io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define close_socket closesocket
#define sleep_us(us) Sleep((DWORD)(((us) + 999u) / 1000u))
#else
#include <netdb.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
static void sleep_us(unsigned us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    (void)nanosleep(&ts, NULL);
}
#endif

int ts_io_startup(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return 1;
#endif
}

void ts_io_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

void ts_input_init_file(TsInput *input, FILE *file) {
    memset(input, 0, sizeof(*input));
    input->kind = TS_IO_FILE;
    input->file = file;
    input->sock = INVALID_SOCKET;
}

static int set_reuseaddr(TsSocket sock) {
    int yes = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes)) == 0;
}

int ts_input_init_udp(TsInput *input, const char *port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int ok = 0;

    memset(input, 0, sizeof(*input));
    input->kind = TS_IO_UDP;
    input->sock = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        return 0;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        TsSocket sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        (void)set_reuseaddr(sock);
        if (bind(sock, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
            input->sock = sock;
            ok = 1;
            break;
        }
        close_socket(sock);
    }

    freeaddrinfo(res);
    return ok;
}

void ts_input_set_poll_callback(TsInput *input, TsInputPollCallback callback, void *userdata) {
    input->poll_callback = callback;
    input->poll_userdata = userdata;
}

size_t ts_input_read(TsInput *input, uint8_t *buf, size_t len) {
    if (input->kind == TS_IO_FILE) {
        return fread(buf, 1, len, input->file);
    }

    size_t out = 0;
    while (out < len) {
        if (input->udp_pos < input->udp_len) {
            size_t available = input->udp_len - input->udp_pos;
            size_t take = available < (len - out) ? available : (len - out);
            memcpy(buf + out, input->udp_buf + input->udp_pos, take);
            input->udp_pos += take;
            out += take;
            continue;
        }

#ifndef _WIN32
        while (1) {
            fd_set fds;
            struct timeval tv;
            FD_ZERO(&fds);
            FD_SET(input->sock, &fds);
            tv.tv_sec = 0;
            tv.tv_usec = 100000;

            int ready = select(input->sock + 1, &fds, NULL, NULL, &tv);
            if (ready > 0) {
                break;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return out;
            }
            if (input->poll_callback) {
                input->poll_callback(input->poll_userdata);
            }
        }
#endif
        int got = recv(input->sock, (char *)input->udp_buf, sizeof(input->udp_buf), 0);
        if (got <= 0) {
            return out;
        }
        if (input->poll_callback) {
            input->poll_callback(input->poll_userdata);
        }
        input->udp_pos = 0;
        input->udp_len = (size_t)got;
    }
    return out;
}

void ts_input_close(TsInput *input) {
    if (input->kind == TS_IO_UDP && input->sock != INVALID_SOCKET) {
        close_socket(input->sock);
        input->sock = INVALID_SOCKET;
    }
}

void ts_output_init_file(TsOutput *output, FILE *file) {
    memset(output, 0, sizeof(*output));
    output->kind = TS_IO_FILE;
    output->file = file;
    output->sock = INVALID_SOCKET;
}

static int split_host_port(const char *host_port, char *host, size_t host_size, char *port, size_t port_size) {
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port || colon[1] == '\0') {
        return 0;
    }
    size_t hlen = (size_t)(colon - host_port);
    if (hlen >= host_size || strlen(colon + 1) >= port_size) {
        return 0;
    }
    memcpy(host, host_port, hlen);
    host[hlen] = '\0';
    strcpy(port, colon + 1);
    return 1;
}

int ts_output_init_udp(TsOutput *output, const char *host_port) {
    char host[256];
    char port[32];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int ok = 0;

    memset(output, 0, sizeof(*output));
    output->kind = TS_IO_UDP;
    output->sock = INVALID_SOCKET;

    if (!split_host_port(host_port, host, sizeof(host), port, sizeof(port))) {
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return 0;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        TsSocket sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        output->sock = sock;
        memcpy(&output->addr, ai->ai_addr, ai->ai_addrlen);
        output->addr_len = (socklen_t)ai->ai_addrlen;
        ok = 1;
        break;
    }

    freeaddrinfo(res);
    return ok;
}

void ts_output_set_udp_rate(TsOutput *output, unsigned kbit_per_sec) {
    output->udp_rate_kbit = kbit_per_sec;
}

static void udp_rate_sleep(TsOutput *output, size_t bytes_sent) {
    if (output->udp_rate_kbit == 0) {
        return;
    }
    uint64_t usec = ((uint64_t)bytes_sent * 8u * 1000u) / output->udp_rate_kbit;
    if (usec > 0) {
        sleep_us((unsigned)usec);
    }
}

static int udp_send_buffer(TsOutput *output) {
    size_t len = output->udp_out_packets * TS_PACKET_SIZE;
    if (len == 0) {
        return 1;
    }
    int sent = sendto(output->sock, (const char *)output->udp_out_buf, len, 0,
                      (const struct sockaddr *)&output->addr, output->addr_len);
    if (sent != (int)len) {
        return 0;
    }
    output->udp_out_packets = 0;
    udp_rate_sleep(output, len);
    return 1;
}

int ts_output_write_packet(TsOutput *output, const uint8_t packet[TS_PACKET_SIZE]) {
    if (output->kind == TS_IO_FILE) {
        return fwrite(packet, 1, TS_PACKET_SIZE, output->file) == TS_PACKET_SIZE;
    }

    memcpy(output->udp_out_buf + output->udp_out_packets * TS_PACKET_SIZE, packet, TS_PACKET_SIZE);
    output->udp_out_packets++;
    if (output->udp_out_packets >= 7) {
        return udp_send_buffer(output);
    }
    return 1;
}

int ts_output_flush(TsOutput *output) {
    if (output->kind == TS_IO_FILE) {
        return fflush(output->file) == 0;
    }
    return udp_send_buffer(output);
}

void ts_output_close(TsOutput *output) {
    (void)ts_output_flush(output);
    if (output->kind == TS_IO_UDP && output->sock != INVALID_SOCKET) {
        close_socket(output->sock);
        output->sock = INVALID_SOCKET;
    }
}
