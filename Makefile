CC ?= cc
CLANG_FORMAT ?= clang-format
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?=

TARGET := ptree
SRC := ptree.c
OBJ := $(SRC:.c=.o)

.PHONY: all test bench fmt clean

all: $(TARGET)

test: $(TARGET)
	python3 tests/run.py discover -s tests -v

bench:
	CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" LDLIBS="$(LDLIBS)" python3 bench/run.py

fmt:
	$(CLANG_FORMAT) --style=file -i $(wildcard *.c *.h bench/*.c bench/*.h)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)
