CC=gcc

CFLAGS=-c -Wall -std=gnu99 -D_XOPEN_SOURCE=500 -D_GNU_SOURCE -O3

#LDFLAGS=-lwiringPi -lrt
LDFLAGS=-lbcm2835 -lrt -lpthread

all: low-energy-meter

low-energy-meter.o: low-energy-meter.c

mcp320x.o: mcp320x.c mcp320x.h

ring.o: ring.h ring.c

low-energy-meter: low-energy-meter.o mcp320x.o ring.o
	$(CC) low-energy-meter.o mcp320x.o ring.o $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -rf low-energy-meter low-energy-meter.o mcp320x.o ring.o
