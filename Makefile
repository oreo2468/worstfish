CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

worstfish: worstfish.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f worstfish

.PHONY: clean
