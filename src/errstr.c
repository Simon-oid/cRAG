/* errstr.c — crag_errstr, shared by the crag program and the library. */
#include "crag.h"

const char *crag_errstr(int err) {
    switch (err) {
        case CRAG_OK:            return "ok";
        case CRAG_EINVAL:        return "invalid argument";
        case CRAG_ENOMEM:        return "out of memory";
        case CRAG_EIO:           return "I/O error";
        case CRAG_EPARSE:        return "parse error";
        case CRAG_EHTTP:         return "HTTP error";
        case CRAG_EDIM_MISMATCH: return "embedding dim mismatch";
        case CRAG_EDB:           return "database error";
        case CRAG_ECONTEXT:      return "input exceeds embedder context";
        default:                 return "unknown error";
    }
}
