CC=gcc
CFLAGS=-Wall -O2 -std=gnu99 -pthread $(shell pkg-config libusb-1.0 --cflags)
LIBS=-lpulse-simple -lfftw3f -lm 

all: monitor usb

monitor: monitor.c
	$(CC) $(CFLAGS) -o monitor monitor.c $(LIBS)

usb: usb.c
	$(CC) $(CFLAGS) -o usb usb.c -lusb-1.0

clean:
	$(RM) monitor
