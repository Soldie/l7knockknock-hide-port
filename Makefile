LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -O2
LIBS = -L. -L$(LIBEVENT)/lib -levent -largp

knock-ssh: knock-ssh.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

knock-ssh-debug: knock-ssh.c
	$(CC) -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -g -o $@ $< $(LIBS)

clean:
	rm -f *.o knock-ssh
