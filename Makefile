CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?=

TARGET := pidtree
SRC := pidtree.c
OBJ := $(SRC:.c=.o)

.PHONY: all test clean

all: $(TARGET)

test: $(TARGET)
	bash test_pidtree.sh
	python3 test_watch.py

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)
