CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Isrc
LDFLAGS ?=
LDLIBS ?=
FFMPEG_PACKAGES := libavcodec libavformat libavutil libswscale
FFMPEG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PACKAGES))
FFMPEG_LIBS = $(shell $(PKG_CONFIG) --libs $(FFMPEG_PACKAGES))
CORE_SRCS := src/scatterfix.c src/io.c src/ts_packet.c src/ts_parser.c src/psi.c src/continuity.c src/ml_model.c src/repair.c src/stats.c src/contest.c
RECOVERY_SRCS := src/recovery.c src/media.c src/rank_model.c
LIB_OBJS := $(CORE_SRCS:.c=.o) $(RECOVERY_SRCS:.c=.o)
CLI_OBJS := src/main.o tools/contest_cli.o tools/contest_main.o tools/model.o
DEPS := $(LIB_OBJS:.o=.d) $(CLI_OBJS:.o=.d)

.PHONY: all core clean test check-deps
all: check-deps libscatterfix.a tsscatterfix scatterfix-contest scatterfix-model
core: libscatterfix-core.a
check-deps:
	@$(PKG_CONFIG) --exists $(FFMPEG_PACKAGES) || { echo 'Install FFmpeg development libraries (libavcodec, libavformat, libavutil, libswscale) and pkg-config, or use make core.' >&2; exit 1; }
libscatterfix.a: $(LIB_OBJS)
	$(AR) rcs $@ $^
libscatterfix-core.a: $(CORE_SRCS:.c=.o)
	$(AR) rcs $@ $^
tsscatterfix: src/main.o tools/contest_cli.o libscatterfix.a
	$(CC) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) -lm -pthread $(LDLIBS)
scatterfix-contest: tools/contest_main.o tools/contest_cli.o libscatterfix.a
	$(CC) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) -lm -pthread $(LDLIBS)
scatterfix-model: tools/model.o libscatterfix.a
	$(CC) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) -lm $(LDLIBS)
examples/embedded: examples/embedded.c libscatterfix.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) -lm $(LDLIBS)
src/media.o: CPPFLAGS += $(FFMPEG_CFLAGS)
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<
tests/test_scatterfix: tests/test_scatterfix.c libscatterfix.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) -lm $(LDLIBS)
test: all tests/test_scatterfix
	./tests/test_scatterfix
	sh tests/test_cli.sh
clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(DEPS) tsscatterfix scatterfix-contest scatterfix-model libscatterfix.a libscatterfix-core.a tests/test_scatterfix examples/embedded
-include $(DEPS)
