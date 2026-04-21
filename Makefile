CC         ?= cc
PKG_CONFIG ?= pkg-config

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -Ithird_party \
           $(CURL_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= $(CURL_LIBS) -lm -lpthread -ldl

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
SQLITE_OBJ := third_party/sqlite3.o
BIN     := crag

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:tests/%.c=tests/%)

all: $(BIN)

$(BIN): $(OBJ) $(SQLITE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

third_party/sqlite3.o: third_party/sqlite3.c
	$(CC) -O2 -w -Ithird_party -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
	      -c -o $@ $<

tests/%: tests/%.c $(filter-out src/main.o, $(OBJ)) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "=== $$t ==="; ./$$t || exit 1; done

clean:
	rm -f $(OBJ) $(SQLITE_OBJ) $(BIN) $(TEST_BIN)

.PHONY: all test clean
