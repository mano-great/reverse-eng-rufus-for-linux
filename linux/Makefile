CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra
LDFLAGS ?=
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin

BIN = rufus-linux
SRC = src/rl_main.c src/rl_device.c src/rl_iso.c src/rl_write.c \
      src/rl_sha256.c src/rl_util.c
OBJ = $(SRC:.c=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_logic: tests/test_logic.c src/rl_iso.c src/rl_sha256.c src/rl_util.c
	$(CC) $(CFLAGS) -o $@ $^

test: tests/test_logic
	./tests/test_logic

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)

uninstall:
	rm -f $(BINDIR)/$(BIN)

clean:
	rm -f $(OBJ) $(BIN) tests/test_logic tests/test_logic.exe

.PHONY: all test install uninstall clean
