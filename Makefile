LIBEVENT ?= /usr/local
CFLAGS = -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -D_GNU_SOURCE
LIBS = -L. -L$(LIBEVENT)/lib 
LIBEV_SOURCES = knock-ssh.c proxy-libevent.c
SPLICE_SOURCES = knock-ssh.c proxy-splice.c

ifeq ($(shell uname), Darwin)
LIBS += -largp
endif

ifdef DEBUG # set with `make .. DEBUG=1`
	CFLAGS+=-g -DDEBUG
ifdef VERBOSE
	CFLAGS+=-DVERY_VERBOSE
endif
else
	CFLAGS+=-O2 -DNDEBUG
endif

knock-ssh-splice: $(SPLICE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SPLICE_SOURCES) $(LIBS) 

knock-ssh: $(LIBEV_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(LIBEV_SOURCES) $(LIBS) -levent

clean:
	rm -f *.o knock-ssh knock-ssh-splice
