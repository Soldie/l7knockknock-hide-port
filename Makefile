LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra
LIBS = -L. -L$(LIBEVENT)/lib -levent 
LIBEV_SOURCES = knock-ssh.c proxy-libevent.c
SPLICE_SOURCES = knock-ssh.c proxy-splice.c

ifeq ($(shell uname), Darwin)
LIBS += -largp
endif

knock-ssh-splice: $(SPLICE_SOURCES)
	$(CC) $(CFLAGS) -O2 -o $@ $(SPLICE_SOURCES) $(LIBS)

knock-ssh: $(LIBEV_SOURCES)
	$(CC) $(CFLAGS) -O2 -o $@ $(LIBEV_SOURCES) $(LIBS)

knock-ssh-debug: $(LIBEV_SOURCES)
	$(CC) $(CFLAGS) -g -o $@ $(LIBEV_SOURCES) $(LIBS)


clean:
	rm -f *.o knock-ssh
