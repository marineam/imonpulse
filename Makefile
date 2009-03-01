CC=gcc
CFLAGS=-Wall -O2 -std=gnu99 -pthread
LIBS=-lpulse-simple -lfftw3f -lm

monitor: monitor.c
	$(CC) $(CFLAGS) -o monitor monitor.c $(LIBS)

clean:
	$(RM) monitor
