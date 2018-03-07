LIBEVENT ?= /usr/local
CFLAGS+= -std=gnu99 -I. -I$(LIBEVENT)/include -Wall -Wpedantic -Wextra -D_GNU_SOURCE
LIBS = -L. -L$(LIBEVENT)/lib 
LIBEV_SOURCES = knock-ssh.c proxy-libevent.c
SPLICE_SOURCES = knock-ssh.c proxy-splice.c

ifdef COVERAGE
CFLAGS+=-O0 -coverage
endif

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

knock-ssh: $(SPLICE_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SPLICE_SOURCES) $(LIBS)

knock-ssh-libevent: $(LIBEV_SOURCES)
	$(CC) $(CFLAGS) -o $@ $(LIBEV_SOURCES) $(LIBS) -levent 


test: knock-ssh
	./run-test.sh ./knock-ssh --valgrind

test-libevent: knock-ssh-libevent
	./run-test.sh ./knock-ssh-libevent --valgrind

clean:
	rm -f *.o *.gcda *.gnco knock-ssh knock-ssh-libevent
