DESTDIR=
PREFIX=/usr/local
BDIR=$(DESTDIR)/$(PREFIX)

.PHONY: all clean install

all: src/tcplex

src/tcplex: src/tcplex.c
	$(MAKE) -C src tcplex

install: src/tcplex
	install -D src/tcplex $(BDIR)/bin/tcplex
	install -m644 -D tcplex.1 $(BDIR)/share/man/man1/tcplex.1

clean:
	$(MAKE) -C src clean
