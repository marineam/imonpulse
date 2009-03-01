CC=gcc
CFLAGS=-Wall -std=gnu99
LIBS=-lpulse-simple -lfftw3f -lm

monitor: monitor.c
	$(CC) $(CFLAGS) -o monitor monitor.c $(LIBS)

clean:
	$(RM) monitor
