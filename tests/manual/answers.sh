#!/usr/bin/env bash
# Level 4: end-to-end ANSWER quality (Italian, bge-m3 + local generation).
#
# The first AUTO-GRADED signal at the answer layer. The rest of the suite
# (recall.sh, gen.sh, eval-ci, BEIR) measures retrieval or the cMCP seam;
# nothing measured whether the assistant's actual ANSWERS are correct and
# grounded. This does, with a scorecard, so it can gate later changes.
#
# For each question in answers_it.tsv it runs query -> retrieve (bge-m3,
# cosine) -> generate (ollama) and grades three things automatically:
#   grounded  the top-1 retrieved doc is in the expected category
#   answered  the model produced an answer (did not spuriously refuse)
#   correct   the answer contains the expected fact (case-insensitive ERE)
# and for out-of-corpus questions, that the model REFUSES ("Not in context.")
# instead of hallucinating.
#
# Usage: tests/manual/answers.sh
#   CRAG_DB             bge-m3 index to query   (default: ~/rag-test.db)
#   CRAG_GEN_MODEL      ollama chat model       (default: qwen2.5:3b)
#   CRAG_GEN_MIN_SCORE  gen confidence gate     (default: 0.38, bge-m3-tuned)
#
# It is a GROSS-FAILURE smoke (the fixture bodies are templated stubs — see
# answers_it.tsv), not a nuanced yardstick. Swap in real Italian docs to
# upgrade the signal; the harness is unchanged.

set -uo pipefail
cd "$(dirname "$0")/../.."

DB="${CRAG_DB:-$HOME/rag-test.db}"
TSV="tests/manual/answers_it.tsv"
CRAG="./crag"
EMBED_URL="${CRAG_EMBED_URL:-http://localhost:11434/api/embeddings}"
GEN_MODEL="${CRAG_GEN_MODEL:-qwen2.5:3b}"
GATE="${CRAG_GEN_MIN_SCORE:-0.38}"      # bge-m3-calibrated; default CLI is 0.60
GEN_DEFAULT_GATE="0.60"                 # the mxbai-era default, for the demo

export CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=bge-m3 CRAG_EMBED_URL="$EMBED_URL"
export CRAG_GEN_BACKEND=ollama CRAG_GEN_MODEL="$GEN_MODEL"

if [ -t 1 ]; then
    B="$(tput bold)"; D="$(tput dim)"; G="$(tput setaf 2)"; Y="$(tput setaf 3)"
    R="$(tput setaf 1)"; C="$(tput setaf 6)"; M="$(tput setaf 5)"; Z="$(tput sgr0)"
else B=""; D=""; G=""; Y=""; R=""; C=""; M=""; Z=""; fi
ok(){ printf '%sPASS%s' "$G" "$Z"; }
no(){ printf '%sFAIL%s' "$R" "$Z"; }

# --- preflight -------------------------------------------------------------
[ -x "$CRAG" ] || make -s >/dev/null
if ! curl -sf --max-time 2 "http://localhost:11434/api/tags" >/dev/null; then
    echo "${R}ollama not reachable at :11434${Z}"; exit 2; fi
if [ ! -f "$DB" ]; then
    echo "${R}no index at $DB${Z} — set CRAG_DB or reindex with bge-m3:"
    echo "  CRAG_EMBED_BACKEND=ollama CRAG_EMBED_MODEL=bge-m3 ./crag reindex ~/rag-test-data --db $DB --url $EMBED_URL"
    exit 2; fi
if ! curl -sf --max-time 2 http://localhost:11434/api/tags | grep -q "\"$GEN_MODEL\""; then
    echo "${R}gen model '$GEN_MODEL' not in ollama${Z} — pull it: ollama pull $GEN_MODEL"; exit 2; fi

echo "${B}=== cRAG end-to-end answer-quality test ===${Z}"
echo "db:    ${C}$DB${Z}    embed: ${C}bge-m3${Z}    gen: ${C}$GEN_MODEL${Z}    gate: ${C}$GATE${Z}"
echo

# Run a cosine query, print top-1 "score<TAB>category". Capture the JSON
# once — two greps on a live pipe would race on the same consumed stdin.
top1() {  # $1=query
    local j sc cat
    j="$("$CRAG" query "$1" --db "$DB" -k 3 --search cosine --format json 2>/dev/null)"
    sc=$(printf '%s' "$j"  | grep -o '"score":[0-9.]*' | head -1 | cut -d: -f2)
    cat=$(printf '%s' "$j" | grep -o '/doc_[0-9]*_[a-z]*\.txt' | head -1 | sed 's/.*_//;s/\.txt//')
    printf '%s\t%s' "${sc:-0}" "${cat:-none}"
}
# Generate an answer (single line), at a given gate.
gen() {  # $1=query  $2=gate
    CRAG_GEN_MIN_SCORE="$2" "$CRAG" query "$1" --db "$DB" -k 3 --search cosine \
        --generate 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//'
}

ng=0; grounded=0; answered=0; correct=0      # answerable tallies
no=0; refused=0                              # ooc tallies
demo_q=""; demo_cos="9"                      # lowest-cosine answerable in [GATE,0.60)

while IFS=$'\t' read -r type cat fact q; do
    case "$type" in ans|ooc) ;; *) continue ;; esac   # skip comments/blanks
    [ -z "${q:-}" ] && continue

    read -r cos rcat < <(printf '%s' "$(top1 "$q")")
    ans="$(gen "$q" "$GATE")"
    is_refusal=0; printf '%s' "$ans" | grep -qi "not in context" && is_refusal=1

    if [ "$type" = "ans" ]; then
        ng=$((ng+1))
        g=0; [ "$rcat" = "$cat" ] && { g=1; grounded=$((grounded+1)); }
        a=0; [ "$is_refusal" -eq 0 ] && { a=1; answered=$((answered+1)); }
        c=0; [ "$a" -eq 1 ] && printf '%s' "$ans" | grep -qiE "$fact" && { c=1; correct=$((correct+1)); }
        # track a query the default 0.60 gate would wrongly refuse
        if [ "$a" -eq 1 ] && awk "BEGIN{exit !($cos>=$GATE && $cos<$GEN_DEFAULT_GATE)}"; then
            if awk "BEGIN{exit !($cos<$demo_cos)}"; then demo_cos="$cos"; demo_q="$q"; fi
        fi
        printf '%s ground=%s ans=%s correct=%s  %scos=%s ->%s%s\n' \
            "$M·$Z" "$([ $g = 1 ] && ok || no)" "$([ $a = 1 ] && ok || no)" \
            "$([ $c = 1 ] && ok || no)" "$D" "$cos" "$rcat" "$Z"
        printf '   %sQ:%s %s\n   %sA:%s %.140s\n\n' "$C" "$Z" "$q" "$D" "$Z" "$ans"
    else
        no=$((no+1))
        r=0; [ "$is_refusal" -eq 1 ] && { r=1; refused=$((refused+1)); }
        printf '%s refuse=%s  %scos=%s%s\n   %sQ:%s %s\n   %sA:%s %.140s\n\n' \
            "$M·$Z" "$([ $r = 1 ] && ok || no)" "$D" "$cos" "$Z" "$C" "$Z" "$q" "$D" "$Z" "$ans"
    fi
done < "$TSV"

# --- scorecard -------------------------------------------------------------
echo "${B}=== scorecard ===${Z}"
printf '  answerable (%d): grounded %s%d/%d%s  answered %s%d/%d%s  correct %s%d/%d%s\n' \
    "$ng" "$B" "$grounded" "$ng" "$Z" "$B" "$answered" "$ng" "$Z" "$B" "$correct" "$ng" "$Z"
printf '  out-of-corpus (%d): refused %s%d/%d%s\n' "$no" "$B" "$refused" "$no" "$Z"

# --- gen-gate calibration demonstration ------------------------------------
# Show that cRAG's generation confidence gate carries the SAME mxbai-era
# default (0.60) the cMCP cosine gate did: a genuine bge-m3 answer with
# cosine in [GATE,0.60) is produced at 0.38 but REFUSED at the 0.60 default.
if [ -n "$demo_q" ]; then
    echo
    echo "${B}=== generation-gate calibration (the same bug, one layer up) ===${Z}"
    echo "${D}genuine query, top-1 cosine $demo_cos — answerable, but in [$GATE, $GEN_DEFAULT_GATE):${Z}"
    echo "  Q: $demo_q"
    a38="$(gen "$demo_q" "$GATE")"; a60="$(gen "$demo_q" "$GEN_DEFAULT_GATE")"
    r60=0; printf '%s' "$a60" | grep -qi "not in context" && r60=1
    printf '  gate %s0.38%s (bge-m3): %.90s\n' "$G" "$Z" "$a38"
    printf '  gate %s0.60%s (default): %s\n' "$Y" "$Z" \
        "$([ $r60 = 1 ] && printf '%sREFUSED — Not in context.%s  <- wrongly drops a genuine answer' "$R" "$Z" || printf '%.90s' "$a60")"
    echo "${D}=> set CRAG_GEN_MIN_SCORE=0.38 for bge-m3, mirroring the cMCP cosine gate.${Z}"
fi
