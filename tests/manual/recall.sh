#!/usr/bin/env bash
# Level 2: compute recall@1, recall@3, and MRR over the golden set.
#
# Usage: tests/manual/recall.sh [backend] [golden_tsv]
#   backend    ollama (default) | openai | local
#   golden_tsv path to a TSV file (default: tests/manual/golden.tsv)
#
# Environment:
#   CRAG_EMBED_MODEL   override embedding model name (recorded in history)
#   CRAG_SAVE_HISTORY  when "1", writes a JSON snapshot to
#                      tests/manual/history/<timestamp>-<model>.json
#
# The script prints per-query rank plus a summary. Failures are listed
# at the bottom for easy inspection.

set -euo pipefail

cd "$(dirname "$0")/../.."

BACKEND="${1:-ollama}"
GOLDEN="${2:-tests/manual/golden.tsv}"
DB="/tmp/crag_manual_recall.db"
CORPUS="tests/manual/corpus"
CRAG="./crag"
TOP_K=5

if [ -t 1 ]; then
    C_BOLD="$(tput bold)"; C_DIM="$(tput dim)"
    C_GRN="$(tput setaf 2)"; C_YLW="$(tput setaf 3)"
    C_RED="$(tput setaf 1)"; C_CYN="$(tput setaf 6)"; C_RST="$(tput sgr0)"
else
    C_BOLD=""; C_DIM=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""; C_RST=""
fi

echo "${C_BOLD}=== cRAG golden recall test ===${C_RST}"
echo "backend: ${C_CYN}${BACKEND}${C_RST}"
echo "golden:  ${GOLDEN}"
echo "top-k:   ${TOP_K}"
echo

if [ ! -x "${CRAG}" ] || [ "src/main.c" -nt "${CRAG}" ]; then
    make -s >/dev/null
fi

if [ "${BACKEND}" = "ollama" ] && ! curl -sf --max-time 2 http://localhost:11434/api/tags >/dev/null; then
    echo "${C_RED}ollama not reachable at :11434${C_RST}"; exit 2
fi

rm -f "${DB}"
CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" index "${CORPUS}" --db "${DB}" >/dev/null

TOTAL=0
HITS_AT_1=0
HITS_AT_3=0
RR_SUM=0          # reciprocal rank * 1000 (we stay in integer arithmetic)
FAILURES=()
RECORDS=()        # per-query TSV rows for JSON history: expected\tquery\trank\ttop_score

while IFS=$'\t' read -r expected query; do
    # Skip comments and blanks
    [ -z "${expected}" ] && continue
    case "${expected}" in \#*) continue ;; esac

    TOTAL=$((TOTAL+1))

    json="$(CRAG_EMBED_BACKEND="${BACKEND}" "${CRAG}" query "${query}" \
            --db "${DB}" -k ${TOP_K} --format json 2>/dev/null || true)"

    top_score="$(echo "${json}" | jq -r '.[0].score // 0' 2>/dev/null || echo 0)"

    # Build ordered list of basenames from the JSON result
    rank=0
    found_rank=0
    while IFS= read -r path; do
        [ -z "${path}" ] && continue
        rank=$((rank+1))
        base="${path##*/}"
        if [ "${base}" = "${expected}" ] && [ "${found_rank}" -eq 0 ]; then
            found_rank=${rank}
        fi
    done < <(echo "${json}" | jq -r '.[].source' 2>/dev/null || true)

    RECORDS+=("${expected}"$'\t'"${query}"$'\t'"${found_rank}"$'\t'"${top_score}")

    # Color per rank bucket
    if   [ "${found_rank}" -eq 1 ]; then
        HITS_AT_1=$((HITS_AT_1+1)); HITS_AT_3=$((HITS_AT_3+1))
        RR_SUM=$((RR_SUM + 1000))
        printf "   %srank %d%s  %s %s%s\n" \
            "${C_GRN}" "${found_rank}" "${C_RST}" "${expected}" "${C_DIM}::${C_RST}" " ${query}"
    elif [ "${found_rank}" -ge 2 ] && [ "${found_rank}" -le 3 ]; then
        HITS_AT_3=$((HITS_AT_3+1))
        RR_SUM=$((RR_SUM + 1000 / found_rank))
        printf "   %srank %d%s  %s %s%s\n" \
            "${C_YLW}" "${found_rank}" "${C_RST}" "${expected}" "${C_DIM}::${C_RST}" " ${query}"
    elif [ "${found_rank}" -ge 4 ]; then
        RR_SUM=$((RR_SUM + 1000 / found_rank))
        printf "   %srank %d%s  %s %s%s\n" \
            "${C_RED}" "${found_rank}" "${C_RST}" "${expected}" "${C_DIM}::${C_RST}" " ${query}"
        FAILURES+=("rank ${found_rank}  ${expected} :: ${query}")
    else
        printf "   %sMISS %s  %s %s%s\n" \
            "${C_RED}" "${C_RST}" "${expected}" "${C_DIM}::${C_RST}" " ${query}"
        FAILURES+=("MISS     ${expected} :: ${query}")
    fi
done < "${GOLDEN}"

echo
if [ "${TOTAL}" -eq 0 ]; then
    echo "${C_RED}no queries parsed from ${GOLDEN}${C_RST}"; exit 2
fi

# Integer-friendly percentage math: (hits * 10000 / total) then format as xx.xx%
pct() {
    local hits="$1" total="$2" bp
    bp=$(( hits * 10000 / total ))
    printf "%d.%02d%%" $((bp / 100)) $((bp % 100))
}
mrr() {
    # RR_SUM is already *1000, divide by total to get MRR*1000
    local v=$(( RR_SUM * 1000 / TOTAL ))
    printf "%d.%03d" $((v / 1000000)) $(((v / 1000) % 1000))
}

echo "${C_BOLD}=== summary over ${TOTAL} queries ===${C_RST}"
printf "  recall@1: %s (%d/%d)\n" "$(pct ${HITS_AT_1} ${TOTAL})" "${HITS_AT_1}" "${TOTAL}"
printf "  recall@3: %s (%d/%d)\n" "$(pct ${HITS_AT_3} ${TOTAL})" "${HITS_AT_3}" "${TOTAL}"
printf "  MRR@${TOP_K}:  %s\n" "$(mrr)"

if [ "${#FAILURES[@]}" -gt 0 ]; then
    echo
    echo "${C_BOLD}failures (inspect these):${C_RST}"
    for f in "${FAILURES[@]}"; do echo "   ${C_RED}${f}${C_RST}"; done
fi

# Optionally save a JSON history snapshot for later trend tracking.
if [ "${CRAG_SAVE_HISTORY:-0}" = "1" ]; then
    hist_dir="tests/manual/history"
    mkdir -p "${hist_dir}"
    model_tag="${CRAG_EMBED_MODEL:-default}"
    # sanitize model tag for filename: replace : and / with -
    safe_tag="$(echo "${model_tag}" | tr ':/' '--')"
    ts="$(date -u +%Y%m%dT%H%M%SZ)"
    hist_file="${hist_dir}/${ts}-${BACKEND}-${safe_tag}.json"

    # Build the per_query JSON array with jq so strings are safely escaped.
    per_query="$(
        for row in "${RECORDS[@]}"; do
            IFS=$'\t' read -r e q r s <<< "${row}"
            jq -n --arg e "$e" --arg q "$q" --argjson r "${r:-0}" \
                   --argjson s "${s:-0}" \
                '{expected: $e, query: $q, rank: $r, top_score: $s}'
        done | jq -s '.'
    )"

    mrr_float="$(awk -v rs="${RR_SUM}" -v t="${TOTAL}" \
        'BEGIN{ printf "%.4f", (t>0 ? rs/1000/t : 0) }')"
    r1_float="$(awk -v h="${HITS_AT_1}" -v t="${TOTAL}" \
        'BEGIN{ printf "%.4f", (t>0 ? h/t : 0) }')"
    r3_float="$(awk -v h="${HITS_AT_3}" -v t="${TOTAL}" \
        'BEGIN{ printf "%.4f", (t>0 ? h/t : 0) }')"

    jq -n \
        --arg ts "${ts}" \
        --arg backend "${BACKEND}" \
        --arg model "${model_tag}" \
        --arg golden "${GOLDEN}" \
        --argjson top_k "${TOP_K}" \
        --argjson total "${TOTAL}" \
        --argjson hits_at_1 "${HITS_AT_1}" \
        --argjson hits_at_3 "${HITS_AT_3}" \
        --argjson recall_at_1 "${r1_float}" \
        --argjson recall_at_3 "${r3_float}" \
        --argjson mrr "${mrr_float}" \
        --argjson per_query "${per_query}" \
        '{
            timestamp: $ts,
            embed_backend: $backend,
            embed_model: $model,
            golden_set: $golden,
            top_k: $top_k,
            totals: { queries: $total, hits_at_1: $hits_at_1, hits_at_3: $hits_at_3 },
            metrics: { recall_at_1: $recall_at_1, recall_at_3: $recall_at_3, mrr: $mrr },
            per_query: $per_query
        }' > "${hist_file}"

    echo
    echo "${C_DIM}saved: ${hist_file}${C_RST}"
fi

# Exit non-zero if recall@3 drops under 80% — advisory, you can ignore
if [ $(( HITS_AT_3 * 100 / TOTAL )) -lt 80 ]; then
    exit 1
fi
