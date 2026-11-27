CC ?= cc
CLANG_FORMAT ?= clang-format
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LDLIBS ?=

TARGET := ptree
SRC := ptree.c
OBJ := $(SRC:.c=.o)
GIT_COMMIT := $(shell git --git-dir=.git rev-parse --short HEAD 2>/dev/null || printf unknown)
BUILD_DATE = $(shell date +%F)
COMMIT_STAMP := .ptree-commit

.PHONY: all test bench fmt clean FORCE

all: $(TARGET)

test: $(TARGET)
	python3 tests/run.py discover -s tests -v

bench:
	CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" LDLIBS="$(LDLIBS)" python3 bench/run.py

fmt:
	$(CLANG_FORMAT) --style=file -i $(wildcard *.c *.h bench/*.c bench/*.h)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ): override CFLAGS += \
	-DPTREE_GIT_COMMIT='"$(GIT_COMMIT)"' \
	-DPTREE_BUILD_DATE='"$(BUILD_DATE)"'

$(OBJ): %.o: %.c $(COMMIT_STAMP) Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

# Refresh the stamp when HEAD changes, even if the source files did not change.
ifneq ($(GIT_COMMIT),$(shell cat $(COMMIT_STAMP) 2>/dev/null))
$(COMMIT_STAMP): FORCE
endif

$(COMMIT_STAMP):
	@printf '%s\n' '$(GIT_COMMIT)' > $@

clean:
	rm -f $(TARGET) $(OBJ) $(COMMIT_STAMP)
