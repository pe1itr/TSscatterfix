CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Isrc
LDFLAGS ?=
LDLIBS ?=

TARGET := tsscatterfix
SRCS := src/main.c src/io.c src/ts_packet.c src/ts_parser.c src/psi.c src/continuity.c src/ml_model.c src/repair.c src/stats.c
OBJS := $(SRCS:.c=.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
