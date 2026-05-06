# Makefile for RDMA RC examples

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -libverbs -lmlx5

BUILD_DIR = build
EXECUTABLES = rc_write rc_send rc_write_write_imm

.PHONY: all clean $(EXECUTABLES)

all: $(EXECUTABLES)

$(EXECUTABLES): %: | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $@.c $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)