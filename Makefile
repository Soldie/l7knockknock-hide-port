CFLAGS+= -std=gnu99 -I. -Wall -Wpedantic -Wextra -D_GNU_SOURCE
LIBS = -L.  
SOURCES = l7knockknock.c proxy-splice.c
MAIN_PROGRAM= l7knockknock

UNAME_S := $(shell uname -s)
ifneq ($(UNAME_S),Linux)
USELIBEVENT = 1
endif


.PHONY: clean test test-libevent

ifdef USELIBEVENT
SOURCES= l7knockknock.c proxy-libevent.c
LIBEVENT ?= /usr/local # if not defined, default to homebrew folder
LIBS+= -L$(LIBEVENT)/lib -levent
CFLAGS+=-I$(LIBEVENT)/include 
endif

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

$(MAIN_PROGRAM): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LIBS)

test: $(MAIN_PROGRAM) 
	./run-test.sh ./$(MAIN_PROGRAM) --valgrind

clean:
	rm -f *.o *.gcda *.gcno $(MAIN_PROGRAM)
