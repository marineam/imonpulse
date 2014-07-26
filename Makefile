CC = gcc
CFLAGS  = -g -Wall -O2 -std=gnu99 -Isrc
CFLAGS += $(shell pkg-config fftw3f --cflags)
CFLAGS += $(shell pkg-config libpulse --cflags)
CFLAGS += $(shell pkg-config libusb-1.0 --cflags)
LIBS  = -lrt -lm -pthread
LIBS += $(shell pkg-config fftw3f --libs)
LIBS += $(shell pkg-config libpulse --libs)
LIBS += $(shell pkg-config libusb-1.0 --libs)

HEADERS = src/monitor.h
SRC_FILES = $(wildcard src/*.c) $(wildcard ccan/*/*.c)
OJB_FILES = $(SRC_FILES:.c=.o)

all: imonpulse

%.o: %.c Makefile $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

imonpulse: $(OJB_FILES)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: imonpulse
	install -m 755 imonpulse /usr/local/bin
	install -m 644 60-imonpulse.rules /etc/udev/rules.d
	install -m 644 imonpulse.service /etc/systemd/system

clean:
	$(RM) $(OJB_FILES) imonpulse

.PHONY: all install clean
