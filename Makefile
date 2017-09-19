LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -O2
LIBS = -L. -L$(LIBEVENT)/lib -levent -largp

https-knock-ssh: https-knock-ssh.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

https-knock-ssh-debug: https-knock-ssh.c
	$(CC) -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -g -o $@ $< $(LIBS)

clean:
	rm -f *.o https-knock-ssh
