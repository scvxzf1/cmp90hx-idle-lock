CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDLIBS := -lnvidia-ml

.PHONY: all clean test

all: cmp90hx-idle-lock

cmp90hx-idle-lock: cmp90hx-idle-lock.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

test: cmp90hx-idle-lock
	./cmp90hx-idle-lock --check

clean:
	rm -f cmp90hx-idle-lock
