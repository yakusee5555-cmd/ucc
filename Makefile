CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Isrc
SRC = $(wildcard src/*.c)
BIN = build/ucc

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(BIN)

clean:
	rm -rf build

.PHONY: all clean
