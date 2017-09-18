LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -g -O2
LIBS = -L. -L$(LIBEVENT)/lib -levent

https-knock-ssh: https-knock-ssh.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)


clean:
	rm -f *.o https-knock-ssh
