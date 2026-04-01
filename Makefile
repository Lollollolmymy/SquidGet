CC   ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -O2 -g 
LDFLAGS := -lcurl -lm
TARGET := squidget

ifneq ($(OS),Windows_NT)
 LDFLAGS += -lpthread
endif

SRCS := main.c api.c download.c tui.c json.c config.c platform.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean windows

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c squidget.h json.h thread.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe

# ── Windows cross-compile (mingw-w64) ─────────────────────────────────
WINCC   ?= x86_64-w64-mingw32-gcc
WINCFLAGS := -std=c11 -Wall -O2
WINLDFLAGS := -lwinhttp -lm -lshell32 -lole32

windows:
	$(WINCC) $(WINCFLAGS) \
	  main.c api.c download.c tui.c json.c config.c platform.c \
	  -I. $(WINLDFLAGS) \
	  -o squidget.exe
	@echo "✓ squidget.exe"
