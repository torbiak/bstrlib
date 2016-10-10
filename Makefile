PREFIX ?= /usr/local
MANPREFIX ?= /usr/local/man
MAKE_MAN_AUX ?= 1

CFLAGS = -std=c99 -g -O2 -fPIC -Werror -Wall -Wextra -rdynamic $(OPTCFLAGS)

SOURCES = $(filter-out %test.c test%.c lex.yy.c manify.c,$(wildcard *.c))
OBJECTS = $(SOURCES:.c=.o)
HEADERS = $(wildcard *.h)

TARGET = libstr.a
TARGET_SO = $(TARGET:.a=.so)

all: $(TARGET_SO) man

$(TARGET): $(OBJECTS) $(HEADERS)
	ar rcs $@
	ranlib $@

$(TARGET_SO): $(TARGET) $(OBJECTS)
	$(CC) -shared -o $@ $(OBJECTS)

man: manify
	rm -rf man3
	mkdir -p man3
	./manify <bstrlib.txt >man3/bstrlib.3
	test $(MAKE_MAN_AUX) -eq 0 || awk -n -f aux_defs.awk bstraux.h | ./manify

manify: manify.c Makefile
	flex manify.c
	$(CC) $(CFLAGS) -Wno-error -lbsd -lfl -o manify lex.yy.c


# The names of all the manpages aren't known in advance, so "sudo make install"
# regenerates all the manpages (owned as root) even if they already existed.
install: all
	install -d $(DESTDIR)$(PREFIX)/lib/
	install $(TARGET) $(TARGET_SO) $(DESTDIR)$(PREFIX)/lib/
	install $(HEADERS) $(DESTDIR)$(PREFIX)/include/
	install -d $(DESTDIR)$(MANPREFIX)/man3
	install -m 644 man3/* $(DESTDIR)$(MANPREFIX)/man3

clean:
	-rm -rf man3
	-rm -f manify lex.yy.c $(OBJECTS) $(TARGET) $(TARGET_SO)

.PHONY: all man install clean
