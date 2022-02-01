CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -Wall -s
PREFIX = /usr/local

chd: chd.o
chd.o: chd.c

clean:
	rm -f chd.o chd

install:
	cp chd $(PREFIX)/bin/
	cp chd.1 $(PREFIX)/share/man/man1/
