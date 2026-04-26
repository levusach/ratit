CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?= -lncursesw
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin

ratit: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(LDFLAGS) -o ratit

install: ratit
	install -Dm755 ratit "$(DESTDIR)$(BINDIR)/ratit"

clean:
	rm -f ratit

.PHONY: install clean

