#!/usr/bin/env bash
# Compare two recall.sh history snapshots and show the delta.
#
# Usage:
#   tests/manual/compare.sh                        # latest vs previous
#   tests/manual/compare.sh <new.json>             # <new> vs previous
#   tests/manual/compare.sh <old.json> <new.json>  # explicit pair
#
# Output:
#   - header: which snapshots are compared
#   - metric deltas: recall@1, recall@3, MRR with +/- sign and color
#   - per-query rank changes: green = improved, red = regressed
#   - new misses / newly-fixed queries listed separately

set -euo pipefail

cd "$(dirname "$0")/../.."

HIST_DIR="tests/manual/history"

if [ -t 1 ]; then
    C_BOLD="$(tput bold)"; C_DIM="$(tput dim)"
    C_GRN="$(tput setaf 2)"; C_YLW="$(tput setaf 3)"
    C_RED="$(tput setaf 1)"; C_CYN="$(tput setaf 6)"; C_RST="$(tput sgr0)"
else
    C_BOLD=""; C_DIM=""; C_GRN=""; C_YLW=""; C_RED=""; C_CYN=""; C_RST=""
fi

if [ ! -d "${HIST_DIR}" ]; then
    echo "${C_RED}no history directory at ${HIST_DIR}${C_RST}"
    echo "run: CRAG_SAVE_HISTORY=1 bash tests/manual/recall.sh"
    exit 2
fi

# Resolve the two snapshots to compare
case "$#" in
    0)
        mapfile -t all < <(ls -1 "${HIST_DIR}"/*.json 2>/dev/null | sort)
        if [ "${#all[@]}" -lt 2 ]; then
            echo "${C_RED}need at least 2 snapshots in ${HIST_DIR}, found ${#all[@]}${C_RST}"
            echo "run recall.sh twice with CRAG_SAVE_HISTORY=1 first"
            exit 2
        fi
        OLD="${all[-2]}"
        NEW="${all[-1]}"
        ;;
    1)
        NEW="$1"
        mapfile -t all < <(ls -1 "${HIST_DIR}"/*.json 2>/dev/null | sort)
        OLD=""
        for f in "${all[@]}"; do
            if [ "${f}" = "${NEW}" ]; then break; fi
            OLD="${f}"
        done
        if [ -z "${OLD}" ]; then
            echo "${C_RED}no snapshot older than ${NEW}${C_RST}"; exit 2
        fi
        ;;
    2)
        OLD="$1"; NEW="$2"
        ;;
    *)
        echo "usage: compare.sh [old.json] [new.json]"; exit 2
        ;;
esac

for f in "${OLD}" "${NEW}"; do
    if [ ! -f "${f}" ]; then echo "${C_RED}missing: ${f}${C_RST}"; exit 2; fi
done

echo "${C_BOLD}=== history diff ===${C_RST}"
printf "  old: %s\n" "$(basename "${OLD}")"
printf "  new: %s\n" "$(basename "${NEW}")"
echo

# --- metric deltas -----------------------------------------------------------

# Pull scalar metrics via jq
read -r old_r1 old_r3 old_mrr old_total old_model old_backend <<< "$(jq -r '[.metrics.recall_at_1, .metrics.recall_at_3, .metrics.mrr, .totals.queries, .embed_model, .embed_backend] | @tsv' "${OLD}")"
read -r new_r1 new_r3 new_mrr new_total new_model new_backend <<< "$(jq -r '[.metrics.recall_at_1, .metrics.recall_at_3, .metrics.mrr, .totals.queries, .embed_model, .embed_backend] | @tsv' "${NEW}")"

if [ "${old_total}" != "${new_total}" ]; then
    echo "${C_YLW}warning: query counts differ (old=${old_total}, new=${new_total})${C_RST}"
    echo "${C_YLW}         comparison may be misleading${C_RST}"
    echo
fi

printf "  %-8s %s → %s\n" "backend:" "${old_backend}" "${new_backend}"
printf "  %-8s %s → %s\n" "model:"   "${old_model}"   "${new_model}"
echo

fmt_delta() {
    # $1=old $2=new $3=label (percentage-like 0.0..1.0) $4=scale (100 for pct, 1 for raw)
    awk -v o="$1" -v n="$2" -v label="$3" -v scale="$4" \
        -v grn="${C_GRN}" -v red="${C_RED}" -v dim="${C_DIM}" -v rst="${C_RST}" '
        BEGIN {
            d = n - o
            if (scale == 100) {
                printf "  %-10s %6.2f%% → %6.2f%%  ", label, o*100, n*100
                if (d > 0.0001)      printf "%s(+%.2f%%)%s\n", grn, d*100, rst
                else if (d < -0.0001) printf "%s(%.2f%%)%s\n", red, d*100, rst
                else                  printf "%s(unchanged)%s\n", dim, rst
            } else {
                printf "  %-10s %6.3f  → %6.3f   ", label, o, n
                if (d > 0.0005)      printf "%s(+%.3f)%s\n", grn, d, rst
                else if (d < -0.0005) printf "%s(%.3f)%s\n", red, d, rst
                else                  printf "%s(unchanged)%s\n", dim, rst
            }
        }'
}

echo "${C_BOLD}metrics:${C_RST}"
fmt_delta "${old_r1}"  "${new_r1}"  "recall@1:" 100
fmt_delta "${old_r3}"  "${new_r3}"  "recall@3:" 100
fmt_delta "${old_mrr}" "${new_mrr}" "MRR:"       1
echo

# --- per-query rank changes --------------------------------------------------

# Join the two per_query arrays on (expected, query). For queries that
# appear in both snapshots, compute rank_old / rank_new and classify.
joined="$(
    jq -s '
        . as $both
        | ($both[0].per_query + $both[1].per_query)
        | group_by(.expected + "|" + .query)
        | map(select(length == 2))
        | map({
            key: (.[0].expected + " :: " + .[0].query),
            old_rank: .[0].rank,
            new_rank: .[1].rank,
            old_score: .[0].top_score,
            new_score: .[1].top_score
          })
    ' "${OLD}" "${NEW}"
)"

# A rank of 0 means MISS (not in top-K). We treat 0 as "infinity" for
# improvement/regression classification.
rank_label() {
    # $1 = rank
    if [ "$1" -eq 0 ]; then echo "MISS"; else echo "#$1"; fi
}

improved_lines=()
regressed_lines=()
fixed_lines=()     # was MISS, now found
new_miss_lines=()  # was found, now MISS

while IFS=$'\t' read -r key old_rank new_rank old_score new_score; do
    old_lbl="$(rank_label "${old_rank}")"
    new_lbl="$(rank_label "${new_rank}")"
    ds="$(awk -v o="${old_score}" -v n="${new_score}" \
            'BEGIN{ printf "%+.3f", n-o }')"

    # classification
    if [ "${old_rank}" -eq 0 ] && [ "${new_rank}" -ne 0 ]; then
        fixed_lines+=("$(printf "  %sMISS → #%s%s  (Δscore %s)  %s" \
            "${C_GRN}" "${new_rank}" "${C_RST}" "${ds}" "${key}")")
    elif [ "${old_rank}" -ne 0 ] && [ "${new_rank}" -eq 0 ]; then
        new_miss_lines+=("$(printf "  %s#%s → MISS%s  (Δscore %s)  %s" \
            "${C_RED}" "${old_rank}" "${C_RST}" "${ds}" "${key}")")
    elif [ "${new_rank}" -lt "${old_rank}" ]; then
        improved_lines+=("$(printf "  %s%s → %s%s  (Δscore %s)  %s" \
            "${C_GRN}" "${old_lbl}" "${new_lbl}" "${C_RST}" "${ds}" "${key}")")
    elif [ "${new_rank}" -gt "${old_rank}" ]; then
        regressed_lines+=("$(printf "  %s%s → %s%s  (Δscore %s)  %s" \
            "${C_RED}" "${old_lbl}" "${new_lbl}" "${C_RST}" "${ds}" "${key}")")
    fi
done < <(echo "${joined}" | jq -r '.[] | [.key, .old_rank, .new_rank, .old_score, .new_score] | @tsv')

print_section() {
    local title="$1"; shift
    local -a lines=("$@")
    if [ "${#lines[@]}" -eq 0 ]; then return; fi
    echo "${C_BOLD}${title}${C_RST} (${#lines[@]})"
    for ln in "${lines[@]}"; do echo "${ln}"; done
    echo
}

print_section "newly fixed (was MISS, now found):"  "${fixed_lines[@]}"
print_section "improved ranks:"                      "${improved_lines[@]}"
print_section "regressed ranks:"                     "${regressed_lines[@]}"
print_section "new misses (was found, now MISS):"    "${new_miss_lines[@]}"

# Only-in-one detection (queries added/removed between snapshots)
only_old="$(jq -s '
    (.[0].per_query | map(.expected + "|" + .query)) as $o
    | (.[1].per_query | map(.expected + "|" + .query)) as $n
    | $o - $n
    | .[]
' "${OLD}" "${NEW}" | wc -l)"
only_new="$(jq -s '
    (.[0].per_query | map(.expected + "|" + .query)) as $o
    | (.[1].per_query | map(.expected + "|" + .query)) as $n
    | $n - $o
    | .[]
' "${OLD}" "${NEW}" | wc -l)"

if [ "${only_old}" -gt 0 ] || [ "${only_new}" -gt 0 ]; then
    echo "${C_DIM}note: ${only_old} queries only in old, ${only_new} queries only in new${C_RST}"
fi
