CC=gcc
CFLAGS=-Wall -std=gnu99
LIBS=-lpulse-simple

monitor: monitor.c
	$(CC) $(CFLAGS) -o monitor monitor.c $(LIBS)

clean:
	$(RM) monitor
