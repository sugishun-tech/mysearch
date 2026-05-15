CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -pthread
LDFLAGS ?= -pthread

all: mysearch

mysearch: mysearch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f mysearch
