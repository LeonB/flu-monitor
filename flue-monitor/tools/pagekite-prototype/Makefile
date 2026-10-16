CFLAGS += -std=c11 -Wall -Wextra -Werror -O2 $(shell pkg-config --cflags openssl)
LDLIBS += $(shell pkg-config --libs openssl)
prototype: prototype.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
clean:
	rm -f prototype
.PHONY: clean
