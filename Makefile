CC=gcc
CFLAGS += -g -Wall -O2 -std=gnu99
CFLAGS += $(shell pkg-config fftw3f --cflags)
CFLAGS += $(shell pkg-config libpulse --cflags)
CFLAGS += $(shell pkg-config libusb-1.0 --cflags)
LIBS += -lrt
LIBS += $(shell pkg-config fftw3f --libs)
LIBS += $(shell pkg-config libpulse --libs)
LIBS += $(shell pkg-config libusb-1.0 --libs)
HEADERS = monitor.h

all: monitor

%.o: %.c Makefile $(HEADERS)
	$(CC) -c $(CFLAGS) -o $@ $<

monitor: imon.o pulse.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) *.o monitor
