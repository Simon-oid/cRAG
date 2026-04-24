#!/usr/bin/env bash
# Diagnostic tool: for a given query, show the rank and score of EVERY
# chunk in the DB — not just top-K. Use this to find where the "right"
# chunk actually ranks when retrieval fails.
#
# Usage:
#   tests/manual/inspect.sh <query>                  # uses the manual-gen DB
#   tests/manual/inspect.sh <query> <source_substr>  # only show chunks whose source matches
#   tests/manual/inspect.sh --dump <source_substr>   # print all chunks of that file (no query)
#
# Examples:
#   tests/manual/inspect.sh "what is the Corpus Juris Civilis"
#   tests/manual/inspect.sh "what is the Corpus Juris Civilis" roman_law
#   tests/manual/inspect.sh --dump roman_law

set -euo pipefail

cd "$(dirname "$0")/../.."

DB="${CRAG_DB:-/tmp/crag_manual_gen.db}"
BACKEND="${CRAG_EMBED_BACKEND:-ollama}"
CRAG="./crag"
CORPUS="tests/manual/corpus"

if [ -t 1 ]; then
    C_BOLD="$(tput bold)"; C_DIM="$(tput dim)"
    C_GRN="$(tput setaf 2)"; C_YLW="$(tput setaf 3)"
    C_RED="$(tput setaf 1)"; C_CYN="$(tput setaf 6)"; C_RST="$(tput sgr0)"
else
    C_BOLD=""; C_DIM=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""; C_RST=""
fi

if [ ! -x "${CRAG}" ]; then make -s >/dev/null; fi

# Lazy-build DB if missing
if [ ! -f "${DB}" ]; then
    echo "${C_DIM}building DB at ${DB}...${C_RST}"
    CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" index "${CORPUS}" --db "${DB}" >/dev/null
fi

# --dump mode: print all chunks of matching files
if [ "${1:-}" = "--dump" ]; then
    filter="${2:-}"
    if [ -z "${filter}" ]; then
        echo "usage: inspect.sh --dump <source_substr>"; exit 2
    fi
    echo "${C_BOLD}=== chunks of source matching '${filter}' in ${DB} ===${C_RST}"
    sqlite3 -separator $'\t' "${DB}" \
        "SELECT id, source, length(text), substr(text, 1, 120)
         FROM chunks WHERE source LIKE '%${filter}%' ORDER BY id;" \
        | while IFS=$'\t' read -r id source len preview; do
            base="${source##*/}"
            printf "${C_CYN}[id=%s]${C_RST} %s ${C_DIM}(%s chars)${C_RST}\n" \
                "${id}" "${base}" "${len}"
            # Preview (first 120 chars), trim to first newline for readability
            printf "   %s\n\n" "${preview}" | tr '\n' ' ' | sed 's/$/\n\n/'
        done
    exit 0
fi

# Default mode: rank all chunks for a query
QUERY="${1:-}"
SRC_FILTER="${2:-}"
if [ -z "${QUERY}" ]; then
    echo "usage: inspect.sh <query> [source_substr]"
    echo "       inspect.sh --dump <source_substr>"
    exit 2
fi

# How many chunks exist total — cap top_k there
TOTAL_CHUNKS="$(sqlite3 "${DB}" "SELECT COUNT(*) FROM chunks;")"
TOP_K=$(( TOTAL_CHUNKS < 500 ? TOTAL_CHUNKS : 500 ))

echo "${C_BOLD}=== inspect: ${QUERY} ===${C_RST}"
echo "${C_DIM}db: ${DB}  total chunks: ${TOTAL_CHUNKS}  backend: ${BACKEND}${C_RST}"
if [ -n "${SRC_FILTER}" ]; then
    echo "${C_DIM}filter: source LIKE %${SRC_FILTER}%${C_RST}"
fi
echo

json="$(CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" query "${QUERY}" \
        --db "${DB}" -k "${TOP_K}" --format json 2>/dev/null)"

if [ -z "${json}" ] || [ "${json}" = "[]" ]; then
    echo "${C_RED}no results${C_RST}"; exit 1
fi

rank=0
shown=0
while IFS=$'\t' read -r score source preview; do
    rank=$((rank+1))
    base="${source##*/}"

    # Apply optional filter
    if [ -n "${SRC_FILTER}" ] && ! echo "${source}" | grep -q "${SRC_FILTER}"; then
        continue
    fi
    shown=$((shown+1))

    # Color by rank
    if   [ "${rank}" -le 3 ];  then color="${C_GRN}"
    elif [ "${rank}" -le 10 ]; then color="${C_YLW}"
    else                            color="${C_RED}"
    fi

    rounded="$(printf '%.4f' "${score}")"
    printf "${color}#%-3d [%s]${C_RST} %s\n   ${C_DIM}%s${C_RST}\n\n" \
        "${rank}" "${rounded}" "${base}" "${preview}"
done < <(echo "${json}" | jq -r '.[] | "\(.score)\t\(.source)\t\(.text | gsub("\n"; " ") | .[0:120])"')

if [ "${shown}" -eq 0 ] && [ -n "${SRC_FILTER}" ]; then
    echo "${C_RED}no chunks matched filter '${SRC_FILTER}'${C_RST}"
fi
