CC               = c99
LIBEVENT_CFLAGS  = `pkg-config --cflags libevent`
LIBEVENT_LDFLAGS = `pkg-config --libs libevent`

WFLAGS  = -Wall -Wextra -Wpedantic -Werror -Wdeclaration-after-statement #-Wwrite-strings
CFLAGS  = $(WFLAGS) $(LIBEVENT_CFLAGS) -pipe -fstack-protector -g -ggdb $(EXTRA_CFLAGS)
LDFLAGS = $(LIBEVENT_LDFLAGS) $(EXTRA_LDFLAGS)
DEPS    = 
OBJS    = tcplex.o

.PHONY: all clean install

all: tcplex

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

tcplex: $(OBJS)
	$(CC) -o $@ $(OBJS) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f tcplex *.o
