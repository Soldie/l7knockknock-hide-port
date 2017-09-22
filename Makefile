LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra
LIBS = -L. -L$(LIBEVENT)/lib -levent -largp
SOURCES = knock-ssh.c proxy-libevent.c

knock-ssh: $(SOURCES)
	$(CC) $(CFLAGS) -O2 -o $@ $(SOURCES) $(LIBS)

knock-ssh-debug: $(SOURCES)
	$(CC) $(CFLAGS) -g -o $@ $(SOURCES) $(LIBS)

clean:
	rm -f *.o knock-ssh
