CC = gcc
CFLAGS = -Wall -Wextra -std=c99

.PHONY: all clean

BIN_DIR = bin

all: $(BIN_DIR)/read_input $(BIN_DIR)/poll_inputs

$(BIN_DIR)/read_input: src/read_input.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN_DIR)/poll_inputs: src/poll_inputs.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)/*
