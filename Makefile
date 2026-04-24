CC         ?= cc
PKG_CONFIG ?= pkg-config

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude -Ithird_party \
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

valgrind: $(TEST_BIN)
	@command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 1; }
	@for t in $(TEST_BIN); do \
	    echo "=== valgrind $$t ==="; \
	    valgrind --leak-check=full --errors-for-leak-kinds=definite \
	             --error-exitcode=1 --quiet ./$$t || exit 1; \
	done

clean:
	rm -f $(OBJ) $(SQLITE_OBJ) $(BIN) $(TEST_BIN)

# Manual quality tests — human-verifiable, not graded by CI.
# Override BACKEND with: make manual-smoke BACKEND=openai
BACKEND ?= ollama

manual-smoke: $(BIN)
	@bash tests/manual/smoke.sh $(BACKEND)

manual-recall: $(BIN)
	@bash tests/manual/recall.sh $(BACKEND)

manual-gen: $(BIN)
	@bash tests/manual/gen.sh $(BACKEND)

# Record a recall snapshot for trend tracking.
# Tag the run with CRAG_EMBED_MODEL=<name> so the JSON filename is meaningful.
manual-record: $(BIN)
	@CRAG_SAVE_HISTORY=1 bash tests/manual/recall.sh $(BACKEND)

# Diff the two most recent snapshots (or pass OLD=... NEW=...).
manual-compare:
	@bash tests/manual/compare.sh $(OLD) $(NEW)

manual: manual-smoke manual-recall manual-gen

.PHONY: all test valgrind clean manual manual-smoke manual-recall manual-gen \
        manual-record manual-compare
