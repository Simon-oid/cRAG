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
	      -DSQLITE_ENABLE_FTS5 \
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

# ---- Sanitizer builds ----------------------------------------------------
# ASan/UBSan find memory-safety + undefined-behaviour bugs that valgrind
# can miss (use-of-uninitialised on tight loops, signed-overflow in
# scoring math, alignment violations on float-array casts). TSan finds
# data races in the embed pool that valgrind cannot see at all.
#
# SQLite is vendored at -O2 -w and built by its own rule that does NOT
# use $(CFLAGS), so the sanitizer flags never reach it. The sanitizer
# runtime is pulled in via the sanitized src/*.o + test objects — this is
# the standard "instrument your code, leave the trusted vendor blob
# alone" pattern.
SAN_BASE_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
                   -fno-omit-frame-pointer -Iinclude -Ithird_party \
                   $(CURL_CFLAGS)
ASAN_CFLAGS := $(SAN_BASE_CFLAGS) -fno-sanitize-recover=all \
               -fsanitize=address,undefined
TSAN_CFLAGS := $(SAN_BASE_CFLAGS) -fsanitize=thread

ASAN_OPTS  := halt_on_error=1:abort_on_error=1:print_stacktrace=1
UBSAN_OPTS := halt_on_error=1:print_stacktrace=1
TSAN_OPTS  := halt_on_error=1:second_deadlock_stack=1

test-asan:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test CFLAGS="$(ASAN_CFLAGS)" \
	    ASAN_OPTIONS="$(ASAN_OPTS)" UBSAN_OPTIONS="$(UBSAN_OPTS)"

test-tsan:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test CFLAGS="$(TSAN_CFLAGS)" \
	    TSAN_OPTIONS="$(TSAN_OPTS)"

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

# ---- Retrieval-quality eval (BEIR SciFact) -------------------------------
# Measures cosine/bm25/hybrid against human relevance judgments and prints
# nDCG@10 / Recall@10 / MRR@10 plus published reference numbers. Needs a
# running embedder (default ollama + mxbai-embed-large). See tests/eval/.
eval: $(BIN) tests/eval_beir
	@bash tests/eval/run.sh

.PHONY: all test valgrind test-asan test-tsan check clean manual manual-smoke \
        manual-recall manual-gen manual-record manual-compare eval
