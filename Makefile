CC      ?= cc
PKG_CONFIG ?= pkg-config
CURL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS     := $(shell $(PKG_CONFIG) --libs libcurl)
SQLITE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sqlite3)
SQLITE_LIBS   := $(shell $(PKG_CONFIG) --libs sqlite3)
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude $(CURL_CFLAGS) $(SQLITE_CFLAGS)
LDFLAGS ?=
LDLIBS  ?= $(CURL_LIBS) $(SQLITE_LIBS) -lm

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := crag

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
