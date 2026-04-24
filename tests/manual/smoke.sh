#!/usr/bin/env bash
# Level 1 smoke test: index the manual corpus, run a battery of queries
# (topical + distractor), and print top-3 results so a human can judge.
#
# Usage:   tests/manual/smoke.sh [backend]
# backend: ollama (default) | openai | local
#   ollama  real embeddings via http://localhost:11434 (nomic-embed-text)
#   openai  needs OPENAI_API_KEY
#   local   HTTP client for a llama.cpp-style server on :8080
#
# Exit codes are advisory: this script is meant to be read by a human,
# not graded by CI.

set -euo pipefail

cd "$(dirname "$0")/../.."

BACKEND="${1:-ollama}"
DB="/tmp/crag_manual_smoke.db"
CORPUS="tests/manual/corpus"
CRAG="./crag"

# ANSI colors, disabled if stdout is not a terminal
if [ -t 1 ]; then
    C_BOLD="$(tput bold)"; C_DIM="$(tput dim)"
    C_GRN="$(tput setaf 2)"; C_YLW="$(tput setaf 3)"
    C_RED="$(tput setaf 1)"; C_CYN="$(tput setaf 6)"; C_RST="$(tput sgr0)"
else
    C_BOLD=""; C_DIM=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""; C_RST=""
fi

echo "${C_BOLD}=== cRAG manual smoke test ===${C_RST}"
echo "backend: ${C_CYN}${BACKEND}${C_RST}"
echo "db:      ${DB}"
echo "corpus:  ${CORPUS}"
echo

# Build if needed
if [ ! -x "${CRAG}" ] || [ "src/main.c" -nt "${CRAG}" ]; then
    echo "building crag..."
    make -s >/dev/null
fi

# Ollama sanity check
if [ "${BACKEND}" = "ollama" ]; then
    if ! curl -sf --max-time 2 http://localhost:11434/api/tags >/dev/null; then
        echo "${C_RED}ollama not reachable at :11434 — start it with 'ollama serve'${C_RST}"
        exit 2
    fi
fi

# Clean and index
rm -f "${DB}"
echo "indexing..."
CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" index "${CORPUS}" --db "${DB}" >/dev/null
"${CRAG}" stats --db "${DB}"
echo

# Queries: label | expected_basename (or NONE for distractors) | text
# Distractors test whether the model wrongly returns a high-confidence match
# for topics not in the corpus.
QUERIES=(
    "topic|pasta_carbonara.md|how do you make carbonara without scrambling the eggs"
    "topic|pasta_carbonara.md|what is the difference between guanciale and pancetta"
    "topic|saturn_rings.md|what are Saturn's rings made of"
    "topic|saturn_rings.md|when did Cassini observe Saturn"
    "topic|linux_scheduler.md|how does CFS pick the next task to run"
    "topic|linux_scheduler.md|what is vruntime"
    "topic|roman_law.md|what is the Corpus Juris Civilis"
    "topic|jazz_history.md|who led the bebop movement"
    "distr|NONE|how do I configure nginx reverse proxy with TLS"
    "distr|NONE|what is the capital of Mongolia"
)

PASS=0; FAIL=0; WARN=0
DISTRACTOR_MAX_SCORE="0.55"  # heuristic for real embeddings

run_query() {
    local kind="$1" expected="$2" text="$3"
    echo "${C_BOLD}Q:${C_RST} ${text}"
    echo "${C_DIM}   expected: ${expected}${C_RST}"

    local json top1_src top1_score verdict
    json="$(CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" query "${text}" \
            --db "${DB}" -k 3 --format json 2>/dev/null)"

    if [ -z "${json}" ] || [ "${json}" = "[]" ]; then
        echo "   ${C_RED}NO RESULTS${C_RST}"
        FAIL=$((FAIL+1)); echo; return
    fi

    local top1_path
    top1_path="$(echo "${json}" | jq -r '.[0].source')"
    top1_src="${top1_path##*/}"
    top1_score="$(echo "${json}" | jq -r '.[0].score')"

    # Print full top-3 so the user can eyeball the ranking
    while IFS=$'\t' read -r score path; do
        local base="${path##*/}"
        local rounded; rounded="$(printf '%.4f' "${score}")"
        if [ "${kind}" = "topic" ] && [ "${base}" = "${expected}" ]; then
            echo "   ${C_GRN}[${rounded}] ${path}${C_RST}"
        else
            echo "   [${rounded}] ${path}"
        fi
    done < <(echo "${json}" | jq -r '.[] | "\(.score)\t\(.source)"')

    # Verdict
    if [ "${kind}" = "topic" ]; then
        if [ "${top1_src}" = "${expected}" ]; then
            verdict="${C_GRN}PASS${C_RST} (top1 correct)"
            PASS=$((PASS+1))
        elif echo "${json}" | jq -r '.[].source' | xargs -I{} basename {} | grep -qx "${expected}"; then
            verdict="${C_YLW}WARN${C_RST} (expected in top-3 but not top-1)"
            WARN=$((WARN+1))
        else
            verdict="${C_RED}FAIL${C_RST} (expected file missing from top-3)"
            FAIL=$((FAIL+1))
        fi
    else
        # distractor: any high-scoring result is suspicious
        if awk -v s="${top1_score}" -v t="${DISTRACTOR_MAX_SCORE}" \
                'BEGIN{exit !(s+0 > t+0)}'; then
            verdict="${C_YLW}WARN${C_RST} (distractor got high score ${top1_score})"
            WARN=$((WARN+1))
        else
            verdict="${C_GRN}PASS${C_RST} (distractor correctly low, ${top1_score})"
            PASS=$((PASS+1))
        fi
    fi
    echo "   ${verdict}"
    echo
}

for line in "${QUERIES[@]}"; do
    IFS='|' read -r kind expected text <<< "${line}"
    run_query "${kind}" "${expected}" "${text}"
done

echo "${C_BOLD}=== summary ===${C_RST}"
echo "  ${C_GRN}PASS:${C_RST} ${PASS}"
echo "  ${C_YLW}WARN:${C_RST} ${WARN}"
echo "  ${C_RED}FAIL:${C_RST} ${FAIL}"
echo
echo "Read the output above carefully — scores and rankings tell you more"
echo "than the pass/fail tally. Distractor thresholds are heuristic."

if [ "${FAIL}" -gt 0 ]; then exit 1; fi
