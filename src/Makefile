CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L

TARGETS = part1 part2 part3

all: $(TARGETS)

part1: part1.c
	$(CC) $(CFLAGS) -o $@ $<

part2: part2.c
	$(CC) $(CFLAGS) -o $@ $< -pthread

part3: part3.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) output.txt

.PHONY: all clean