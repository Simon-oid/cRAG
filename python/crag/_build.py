"""cffi build script: compiles cRAG's index (and the vendored SQLite) into
the `crag._native` extension, so a wheel needs nothing from the system.

API mode (set_source + C compiler) rather than ABI mode (dlopen): the
compiler checks the declarations below against crag_lib.h, so a header
change that breaks the binding fails the build instead of crashing at
run time.
"""

from cffi import FFI

# Paths are relative to the repository root, where setup.py runs the build
# (setuptools refuses absolute source paths).

# Mirrors include/crag_lib.h (the compiler checks it against the header).
CDEF = """
typedef struct crag_db crag_db_t;
typedef enum { CRAG_MODE_COSINE = 0, CRAG_MODE_BM25 = 1, CRAG_MODE_HYBRID = 2 } crag_mode_t;
typedef struct {
    long long id;
    char     *source;
    char     *text;
    float     score;
    float     cosine;
    float     bm25;
} crag_hit_t;

int  crag_db_open(const char *path, crag_db_t **out);
void crag_db_close(crag_db_t *db);
int  crag_db_add(crag_db_t *db, const char *source, const char *text,
                 const float *vec, int dim);
int  crag_db_begin(crag_db_t *db);
int  crag_db_commit(crag_db_t *db);
int  crag_db_search(crag_db_t *db, crag_mode_t mode,
                    const float *qvec, int qdim, const char *qtext, int k,
                    crag_hit_t **out_hits, int *out_n);
void crag_hits_free(crag_hit_t *hits, int n);
int  crag_db_stats(crag_db_t *db, int *chunks, int *sources, int *dim);
const char *crag_errstr(int err);

#define CRAG_EMBED_DIM_MAX ...
"""

ffibuilder = FFI()
ffibuilder.cdef(CDEF)
ffibuilder.set_source(
    "crag._native",
    '#include "crag_lib.h"',
    sources=["src/lib.c", "src/store.c", "src/search.c", "src/errstr.c",
             "third_party/sqlite3.c"],
    include_dirs=["include", "third_party"],
    # Same SQLite configuration as the Makefile's build of the crag program.
    define_macros=[
        ("SQLITE_THREADSAFE", "0"),
        ("SQLITE_DEFAULT_WAL_SYNCHRONOUS", "1"),
        ("SQLITE_ENABLE_FTS5", None),
    ],
    extra_compile_args=["-std=c11", "-O2"],
    libraries=["m"],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
