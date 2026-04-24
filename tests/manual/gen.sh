#!/usr/bin/env bash
# Level 3: inspect generation quality.
#
# For each query in a small question set, print:
#   - the top retrieved chunks (the context)
#   - the full prompt that would be sent to an LLM
#   - the actual generated answer (if a generation backend is available)
#
# This lets you judge two things separately:
#   1. Is the retrieved context sufficient to answer the question?
#      (a retrieval problem)
#   2. Does the model actually use the context correctly, without
#      hallucinating beyond it? (a generation problem)
#
# Usage: tests/manual/gen.sh [embed_backend]
#   embed_backend  ollama (default) | openai | local
#
# The generation backend is picked from CRAG_GEN_BACKEND + the relevant
# env (ANTHROPIC_API_KEY / OPENAI_API_KEY / local server at :8080).
# If none is reachable, the script runs in dry-run mode and only
# prints the context.

set -euo pipefail

cd "$(dirname "$0")/../.."

EMBED_BACKEND="${1:-ollama}"
DB="/tmp/crag_manual_gen.db"
CORPUS="tests/manual/corpus"
CRAG="./crag"

if [ -t 1 ]; then
    C_BOLD="$(tput bold)"; C_DIM="$(tput dim)"
    C_GRN="$(tput setaf 2)"; C_YLW="$(tput setaf 3)"
    C_RED="$(tput setaf 1)"; C_CYN="$(tput setaf 6)"; C_MAG="$(tput setaf 5)"; C_RST="$(tput sgr0)"
else
    C_BOLD=""; C_DIM=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""; C_MAG=""; C_RST=""
fi

# Decide whether generation can actually run
GEN_MODE="dry-run"
GEN_BACKEND="${CRAG_GEN_BACKEND:-}"
GEN_MODEL="${CRAG_GEN_MODEL:-}"

# Return a non-embedding ollama model name if one is pulled, else empty
ollama_first_gen_model() {
    curl -sf --max-time 2 http://localhost:11434/api/tags 2>/dev/null \
        | jq -r '.models[]?.name' 2>/dev/null \
        | grep -vE '(embed|bge|minilm)' | head -n1
}

if [ "${GEN_BACKEND}" = "anthropic" ] && [ -n "${ANTHROPIC_API_KEY:-}" ]; then
    GEN_MODE="live"
elif [ "${GEN_BACKEND}" = "openai" ] && [ -n "${OPENAI_API_KEY:-}" ]; then
    GEN_MODE="live"
elif [ "${GEN_BACKEND}" = "ollama" ]; then
    if [ -z "${GEN_MODEL}" ]; then GEN_MODEL="$(ollama_first_gen_model)"; fi
    if [ -n "${GEN_MODEL}" ]; then GEN_MODE="live"; export CRAG_GEN_MODEL="${GEN_MODEL}"; fi
elif [ "${GEN_BACKEND}" = "local" ] && curl -sf --max-time 1 http://localhost:8080/ >/dev/null 2>&1; then
    GEN_MODE="live"
elif [ -z "${GEN_BACKEND}" ]; then
    # auto-detect
    if [ -n "${ANTHROPIC_API_KEY:-}" ]; then
        export CRAG_GEN_BACKEND=anthropic; GEN_BACKEND=anthropic; GEN_MODE="live"
    elif [ -n "${OPENAI_API_KEY:-}" ]; then
        export CRAG_GEN_BACKEND=openai; GEN_BACKEND=openai; GEN_MODE="live"
    else
        GEN_MODEL="$(ollama_first_gen_model)"
        if [ -n "${GEN_MODEL}" ]; then
            export CRAG_GEN_BACKEND=ollama; GEN_BACKEND=ollama; GEN_MODE="live"
            export CRAG_GEN_MODEL="${GEN_MODEL}"
        elif curl -sf --max-time 1 http://localhost:8080/ >/dev/null 2>&1; then
            export CRAG_GEN_BACKEND=local; GEN_BACKEND=local; GEN_MODE="live"
        fi
    fi
fi

echo "${C_BOLD}=== cRAG generation-quality test ===${C_RST}"
echo "embed backend: ${C_CYN}${EMBED_BACKEND}${C_RST}"
if [ "${GEN_MODE}" = "live" ]; then
    if [ -n "${GEN_MODEL}" ]; then
        echo "gen   backend: ${C_GRN}${GEN_BACKEND}${C_RST} (live, model: ${GEN_MODEL})"
    else
        echo "gen   backend: ${C_GRN}${GEN_BACKEND}${C_RST} (live)"
    fi
else
    echo "gen   backend: ${C_YLW}none configured — dry-run mode${C_RST}"
    echo "               ${C_DIM}options to run live:${C_RST}"
    echo "               ${C_DIM}  - pull an ollama gen model: ollama pull llama3.2:3b${C_RST}"
    echo "               ${C_DIM}  - set ANTHROPIC_API_KEY or OPENAI_API_KEY${C_RST}"
    echo "               ${C_DIM}  - start a llama.cpp server on :8080${C_RST}"
fi
echo "corpus:        ${CORPUS}"
echo

if [ ! -x "${CRAG}" ] || [ "src/main.c" -nt "${CRAG}" ]; then
    make -s >/dev/null
fi

if [ "${EMBED_BACKEND}" = "ollama" ] && ! curl -sf --max-time 2 http://localhost:11434/api/tags >/dev/null; then
    echo "${C_RED}ollama not reachable at :11434${C_RST}"; exit 2
fi

rm -f "${DB}"
CRAG_EMBED_BACKEND="${EMBED_BACKEND}" "${CRAG}" index "${CORPUS}" --db "${DB}" >/dev/null

# Each entry: the "right" answer you should see in the generated text.
# We don't grade automatically — we show the expected fact so you can
# glance at the model's output and verify it didn't drift.
QUERIES=(
    "What are Saturn's rings made of?|mostly water ice (90-95%)"
    "Why shouldn't you use cream in a carbonara?|it's not authentic / Roman tradition forbids it"
    "What is vruntime in the Linux CFS scheduler?|virtual runtime — how much CPU time a task has consumed, weighted"
    "What is the Corpus Juris Civilis?|Justinian's 6th-century compilation of Roman law (Codex, Digest, Institutes, Novellae)"
    "Who is associated with the bebop movement?|Charlie Parker and Dizzy Gillespie"
    "What's the capital of Mongolia?|NOT IN CORPUS — model should refuse or say it doesn't know"
)

for entry in "${QUERIES[@]}"; do
    IFS='|' read -r query expected <<< "${entry}"

    echo "${C_BOLD}${C_MAG}=== Q: ${query} ===${C_RST}"
    echo "${C_DIM}expected: ${expected}${C_RST}"
    echo

    # Always show retrieved context first
    echo "${C_CYN}--- retrieved context (top 3) ---${C_RST}"
    CRAG_EMBED_BACKEND="${EMBED_BACKEND}" "${CRAG}" query "${query}" \
        --db "${DB}" -k 3 --format text 2>/dev/null \
        | sed -E "s/^\[/${C_DIM}[/; s/$/${C_RST}/"
    echo

    if [ "${GEN_MODE}" = "live" ]; then
        echo "${C_GRN}--- generated answer ---${C_RST}"
        CRAG_EMBED_BACKEND="${EMBED_BACKEND}" "${CRAG}" query "${query}" \
            --db "${DB}" -k 3 --generate 2>&1 || echo "${C_RED}(generation failed)${C_RST}"
    else
        echo "${C_YLW}--- dry-run: model would answer from the context above ---${C_RST}"
        echo "${C_DIM}judge yourself: could a good LLM answer '${expected}' from these chunks?${C_RST}"
    fi

    echo
    echo "${C_DIM}────────────────────────────────────────────────────────────${C_RST}"
    echo
done

echo "${C_BOLD}=== review checklist ===${C_RST}"
echo "  - for each topical query, is the needed fact actually IN the"
echo "    retrieved context? if not, that's a retrieval failure."
echo "  - for live-mode answers, does the model stick to the context or"
echo "    invent facts? does it refuse on the out-of-corpus question?"
echo "  - for dry-run, mentally simulate: given only these chunks, could"
echo "    a good LLM produce the expected answer?"
