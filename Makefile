CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?=

TARGET := ptree
SRC := ptree.c
OBJ := $(SRC:.c=.o)

.PHONY: all test clean

all: $(TARGET)

test: $(TARGET)
	python3 tests/run.py discover -s tests -v

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)
