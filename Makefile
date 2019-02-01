CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -Wall -s

chd: chd.o
chd.o: chd.c

clean:
	rm -f chd.o chd
