CC = cc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -Iinclude
LDLIBS = -lsqlite3 -lcrypto

SRC = src/server.c src/chat_db.c src/websocket.c
OBJ = $(SRC:src/%.c=build/%.o)
BIN = build/server

server: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJ) $(LDLIBS)

build/%.o: src/%.c include/app.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: clean
