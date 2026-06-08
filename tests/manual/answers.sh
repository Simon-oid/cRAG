#!/usr/bin/env bash
# Level 4: end-to-end ANSWER quality (Italian, bge-m3 + local generation).
#
# The first AUTO-GRADED signal at the answer layer. The rest of the suite
# (recall.sh, gen.sh, eval-ci, BEIR) measures retrieval or the cMCP seam;
# nothing measured whether the assistant's actual ANSWERS are correct and
# grounded. This does, with a scorecard, so it can gate later changes.
#
# For each question in answers_it.tsv it runs query -> retrieve (bge-m3,
# cosine) -> generate (ollama) and grades five things automatically:
#   grounded  the top-1 retrieved doc IS the expected document (by basename)
#   answered  the model produced an answer (did not spuriously refuse)
#   correct   the answer contains the expected fact (case-insensitive ERE)
#   faithful  every claim in the answer is supported by the retrieved context
#             (the RAG-triad "Faithfulness" metric), graded by an LLM judge at
#             temperature 0 so the verdict is reproducible. Needs jq; auto-
#             skipped if absent. Distinct from `correct`: `correct` checks the
#             answer against a hand-written expected fact, `faithful` checks it
#             against what was ACTUALLY retrieved — catching a right-sounding
#             answer the context never supported (hallucination), and the
#             converse (a faithful answer the regex just missed).
#   relevant  the answer DIRECTLY and completely addresses the question, with no
#             digression, padding, or partial dodge (the RAG-triad "Answer
#             Relevancy" metric — the third Triad vertex, orthogonal to the two
#             above: faithful=grounded-in-context, correct=matches-the-fact,
#             relevant=actually-answers-what-was-asked). Same LLM-judge-at-temp-0
#             pattern, judging ANSWER-vs-QUESTION (not the context). jq-gated.
#             NOTE (measured 2026-06-08): relevancy is a SUBTLER judgment than
#             faithfulness and needs a CAPABLE judge. qwen2.5:3b is unreliable
#             as the relevancy judge — it false-flags squarely-on-target answers
#             as IRRELEVANT (3b judge: 40/65; 7b judge on the same answers: far
#             higher). Use CRAG_JUDGE_MODEL=qwen2.5:7b (or stronger) for a
#             trustworthy relevancy number; a weak judge UNDER-reports it.
# and for out-of-corpus questions, that the model REFUSES ("Not in context.")
# instead of hallucinating.
#
# By default it indexes the bundled real corpus (tests/manual/corpus_it/, the
# Vallenova Smart City docs — distinct fact-bearing documents, a real
# yardstick) into a temp DB, so the test is self-contained and reproducible.
# Point CRAG_DB at your own bge-m3 index to grade a different corpus instead.
#
# Usage: tests/manual/answers.sh
#   CRAG_DB             grade THIS bge-m3 index instead of the bundled corpus
#   CRAG_GEN_MODEL      ollama chat model       (default: qwen2.5:3b)
#   CRAG_GEN_MIN_SCORE  gen confidence gate     (default: 0.38, bge-m3-tuned)
#   CRAG_JUDGE_MODEL    faithfulness/relevancy judge model (default: CRAG_GEN_MODEL)
#   CRAG_FAITHFULNESS   set 0 to skip the LLM faithfulness judge (default: 1)
#   CRAG_RELEVANCY      set 0 to skip the LLM answer-relevancy judge (default: 1)

set -uo pipefail
cd "$(dirname "$0")/../.."

CORPUS="tests/manual/corpus_it"
TSV="tests/manual/answers_it.tsv"
CRAG="./crag"
EMBED_URL="${CRAG_EMBED_URL:-http://localhost:11434/api/embeddings}"
GEN_MODEL="${CRAG_GEN_MODEL:-qwen2.5:3b}"
GATE="${CRAG_GEN_MIN_SCORE:-0.38}"      # bge-m3-calibrated; default CLI is 0.60
GEN_DEFAULT_GATE="0.60"                 # the mxbai-era default, for the demo
JUDGE_MODEL="${CRAG_JUDGE_MODEL:-$GEN_MODEL}"   # faithfulness/relevancy LLM judge
FAITH="${CRAG_FAITHFULNESS:-1}"         # 1 = run the faithfulness judge
REL="${CRAG_RELEVANCY:-1}"              # 1 = run the answer-relevancy judge
HAVE_JQ=0; command -v jq >/dev/null 2>&1 && HAVE_JQ=1
[ "$HAVE_JQ" -eq 1 ] || { FAITH=0; REL=0; }   # judges need jq to marshal JSON safely

# Use the caller's index if given, else (re)build one from the bundled corpus.
DB="${CRAG_DB:-/tmp/crag_answers_it.db}"
SELF_CORPUS=0; [ -z "${CRAG_DB:-}" ] && SELF_CORPUS=1

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
if ! curl -sf --max-time 2 http://localhost:11434/api/tags | grep -q "\"$GEN_MODEL\""; then
    echo "${R}gen model '$GEN_MODEL' not in ollama${Z} — pull it: ollama pull $GEN_MODEL"; exit 2; fi
if [ "$SELF_CORPUS" -eq 1 ]; then
    [ -d "$CORPUS" ] || { echo "${R}corpus not found: $CORPUS${Z}"; exit 2; }
    rm -f "$DB"
    "$CRAG" index "$CORPUS" --db "$DB" --ext md >/dev/null \
        || { echo "${R}indexing $CORPUS failed${Z}"; exit 2; }
elif [ ! -f "$DB" ]; then
    echo "${R}no index at CRAG_DB=$DB${Z}"; exit 2; fi

echo "${B}=== cRAG end-to-end answer-quality test ===${Z}"
src_label="$([ "$SELF_CORPUS" -eq 1 ] && echo "$CORPUS (bundled)" || echo "$DB")"
echo "corpus: ${C}$src_label${Z}    embed: ${C}bge-m3${Z}    gen: ${C}$GEN_MODEL${Z}    gate: ${C}$GATE${Z}"
if [ "$FAITH" -eq 1 ] || [ "$REL" -eq 1 ]; then
    echo "RAG-triad judges: ${C}$JUDGE_MODEL${Z} ${D}(temp 0)${Z}  faithfulness ${C}$([ "$FAITH" -eq 1 ] && echo on || echo off)${Z}  relevancy ${C}$([ "$REL" -eq 1 ] && echo on || echo off)${Z}"
else echo "RAG-triad judges: ${Y}off${Z}${D}$([ "$HAVE_JQ" -eq 1 ] || echo ' (jq not found)')${Z}"; fi
echo

# Run a cosine query, print top-1 "score<TAB>doc-stem". Capture the JSON
# once — two greps on a live pipe would race on the same consumed stdin.
# The stem is the source basename without extension (e.g. ".../ztl.md" -> ztl).
top1() {  # $1=query
    local j sc src stem
    j="$("$CRAG" query "$1" --db "$DB" -k 3 --search cosine --format json 2>/dev/null)"
    sc=$(printf '%s' "$j"  | grep -o '"score":[0-9.]*' | head -1 | cut -d: -f2)
    src=$(printf '%s' "$j" | grep -o '"source":"[^"]*"' | head -1 | sed 's/.*"source":"//; s/".*//')
    stem=$(basename "$src" 2>/dev/null | sed 's/\.[^.]*$//')
    printf '%s\t%s' "${sc:-0}" "${stem:-none}"
}
# Generate an answer (single line), at a given gate.
gen() {  # $1=query  $2=gate
    CRAG_GEN_MIN_SCORE="$2" "$CRAG" query "$1" --db "$DB" -k 3 --search cosine \
        --generate 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//'
}
# The exact context the generator sees: the k=3 retrieved chunk texts. The text
# format prepends a "[score ...] <path>" header to each result; strip those so
# the judge sees only retrieved material, the same way query.c concatenates
# results[i].text into the prompt.
ctx_of() {  # $1=query
    "$CRAG" query "$1" --db "$DB" -k 3 --search cosine --format text 2>/dev/null \
        | grep -vE '^\[[0-9-]'
}
# LLM-as-judge faithfulness (RAG-triad "Faithfulness"): is every factual claim
# in the ANSWER supported by the retrieved CONTEXT? Temperature 0 keeps the
# verdict reproducible. jq marshals arbitrary chunk text into the request body
# (quotes/newlines/backslashes) and pulls the reply out — no fragile escaping.
# Returns 0 = faithful (grounded), 1 = unsupported, 2 = judge unavailable.
JUDGE_SYS='You are a strict grader for retrieval-augmented answers. You are given a CONTEXT and an ANSWER. Judge ONLY faithfulness, not correctness: does every factual claim in the ANSWER appear in the CONTEXT? Ignore whether the CONTEXT actually answers the question. If the ANSWER makes no factual claim — it refuses, says the information is not available, or is empty — reply GROUNDED (no claim can be unsupported). Otherwise reply GROUNDED if every claim is supported by the CONTEXT, or UNSUPPORTED if any claim is missing from it. Reply with EXACTLY one word: GROUNDED or UNSUPPORTED.'
judge_faithful() {  # $1=context  $2=answer
    [ "$FAITH" -eq 1 ] || return 2
    local usr body resp
    usr="$(printf 'CONTEXT:\n%s\n\nANSWER:\n%s' "$1" "$2")"
    body="$(jq -n --arg m "$JUDGE_MODEL" --arg s "$JUDGE_SYS" --arg p "$usr" \
        '{model:$m, system:$s, prompt:$p, stream:false, options:{temperature:0}}')" || return 2
    resp="$(curl -sf --max-time 30 http://localhost:11434/api/generate -d "$body" 2>/dev/null \
        | jq -r '.response // empty' 2>/dev/null)"
    [ -z "$resp" ] && return 2
    # "UNSUPPORTED" lacks the substring "grounded"; the (^|[^a-z]) guard also
    # rejects a stray "ungrounded" so only a real GROUNDED verdict passes.
    printf '%s' "$resp" | grep -qiE '(^|[^a-z])grounded' && return 0
    return 1
}
# LLM-as-judge answer-relevancy (RAG-triad "Answer Relevancy"): does the ANSWER
# directly and completely address the QUESTION, with no digression, padding, or
# partial dodge? This is orthogonal to faithfulness — it compares answer-vs-
# question, NOT answer-vs-context — so a perfectly grounded answer that talks
# around the question still scores irrelevant. Temp 0 for reproducibility.
# Returns 0 = relevant, 1 = irrelevant/partial, 2 = judge unavailable.
JUDGE_REL_SYS='You are a strict grader for question-answering. You are given a QUESTION and an ANSWER. Judge ONLY relevancy: does the ANSWER directly and completely address what the QUESTION asks? Ignore whether the answer is factually true. Penalize digressions, padding, hedging, and partial answers that dodge the actual question. An ANSWER that explicitly declines because the information is unavailable is RELEVANT (it correctly addresses an unanswerable question). Reply RELEVANT if the answer squarely addresses the question, or IRRELEVANT if it does not. Reply with EXACTLY one word: RELEVANT or IRRELEVANT.'
judge_relevant() {  # $1=question  $2=answer
    [ "$REL" -eq 1 ] || return 2
    local usr body resp
    usr="$(printf 'QUESTION:\n%s\n\nANSWER:\n%s' "$1" "$2")"
    body="$(jq -n --arg m "$JUDGE_MODEL" --arg s "$JUDGE_REL_SYS" --arg p "$usr" \
        '{model:$m, system:$s, prompt:$p, stream:false, options:{temperature:0}}')" || return 2
    resp="$(curl -sf --max-time 30 http://localhost:11434/api/generate -d "$body" 2>/dev/null \
        | jq -r '.response // empty' 2>/dev/null)"
    [ -z "$resp" ] && return 2
    # "IRRELEVANT" contains the substring "relevant"; anchor on the full word so
    # only a real RELEVANT verdict passes (IRRELEVANT is rejected by the prefix).
    printf '%s' "$resp" | grep -qiE '(^|[^a-z])relevant' && return 0
    return 1
}

ng=0; grounded=0; answered=0; correct=0      # answerable tallies
faithful=0; fjudged=0                         # faithfulness: grounded / judged
relevant=0; rjudged=0                         # answer-relevancy: relevant / judged
no=0; refused=0                              # ooc tallies
nt=0; nabst=0                                # within-corpus-unanswerable tallies
demo_q=""; demo_cos="9"                      # lowest-cosine answerable in [GATE,0.60)

# A response counts as "abstained" if it declines instead of inventing a fact —
# the CLI refusal, or the model itself saying the detail isn't in the context.
ABSTAIN_RE='not in context|non (è |e |viene )?(specificat|indicat|riportat|present|precisat|fornit|menzionat|disponibil)|nessun. informazion'

while IFS=$'\t' read -r type cat fact q; do
    case "$type" in ans|ooc|nca) ;; *) continue ;; esac   # skip comments/blanks
    [ -z "${q:-}" ] && continue

    read -r cos rcat < <(printf '%s' "$(top1 "$q")")
    ans="$(gen "$q" "$GATE")"
    is_refusal=0; printf '%s' "$ans" | grep -qi "not in context" && is_refusal=1

    if [ "$type" = "ans" ]; then
        ng=$((ng+1))
        g=0; [ "$rcat" = "$cat" ] && { g=1; grounded=$((grounded+1)); }
        a=0; [ "$is_refusal" -eq 0 ] && { a=1; answered=$((answered+1)); }
        c=0; [ "$a" -eq 1 ] && printf '%s' "$ans" | grep -qiE "$fact" && { c=1; correct=$((correct+1)); }
        # Faithfulness: judge the produced answer against the retrieved context.
        # Only meaningful when the model actually answered (a refusal is graded
        # by `answered`, not here). '—' when answered but the judge is off/down.
        fcell="${D}—${Z}"; rcell="${D}—${Z}"
        if [ "$a" -eq 1 ]; then
            judge_faithful "$(ctx_of "$q")" "$ans"; fr=$?
            if   [ "$fr" -eq 0 ]; then fcell="$(ok)"; faithful=$((faithful+1)); fjudged=$((fjudged+1))
            elif [ "$fr" -eq 1 ]; then fcell="$(no)"; fjudged=$((fjudged+1)); fi
            # Answer Relevancy: judge the answer against the QUESTION (not ctx).
            judge_relevant "$q" "$ans"; rr=$?
            if   [ "$rr" -eq 0 ]; then rcell="$(ok)"; relevant=$((relevant+1)); rjudged=$((rjudged+1))
            elif [ "$rr" -eq 1 ]; then rcell="$(no)"; rjudged=$((rjudged+1)); fi
        fi
        # track a query the default 0.60 gate would wrongly refuse
        if [ "$a" -eq 1 ] && awk "BEGIN{exit !($cos>=$GATE && $cos<$GEN_DEFAULT_GATE)}"; then
            if awk "BEGIN{exit !($cos<$demo_cos)}"; then demo_cos="$cos"; demo_q="$q"; fi
        fi
        printf '%s ground=%s ans=%s correct=%s faith=%s relev=%s  %scos=%s ->%s%s\n' \
            "$M·$Z" "$([ $g = 1 ] && ok || no)" "$([ $a = 1 ] && ok || no)" \
            "$([ $c = 1 ] && ok || no)" "$fcell" "$rcell" "$D" "$cos" "$rcat" "$Z"
        printf '   %sQ:%s %s\n   %sA:%s %.140s\n\n' "$C" "$Z" "$q" "$D" "$Z" "$ans"
    elif [ "$type" = "nca" ]; then
        # Within-corpus unanswerable: the right doc IS retrieved, but the asked
        # fact isn't in it. A good system abstains; a hallucinator invents.
        nt=$((nt+1))
        ab=0; printf '%s' "$ans" | grep -qiE "$ABSTAIN_RE" && { ab=1; nabst=$((nabst+1)); }
        printf '%s abstain=%s  %scos=%s ->%s (fact not in corpus)%s\n   %sQ:%s %s\n   %sA:%s %.140s\n\n' \
            "$M·$Z" "$([ $ab = 1 ] && ok || no)" "$D" "$cos" "$rcat" "$Z" "$C" "$Z" "$q" "$D" "$Z" "$ans"
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
if [ "$FAITH" -eq 1 ]; then
    printf '  faithfulness (judged %d of %d answered): grounded %s%d/%d%s\n' \
        "$fjudged" "$answered" "$B" "$faithful" "$fjudged" "$Z"
else
    printf '  faithfulness: %sskipped%s (set CRAG_FAITHFULNESS=1%s)\n' \
        "$Y" "$Z" "$([ "$HAVE_JQ" -eq 1 ] && echo "" || echo " and install jq")"
fi
if [ "$REL" -eq 1 ]; then
    printf '  answer relevancy (judged %d of %d answered): relevant %s%d/%d%s\n' \
        "$rjudged" "$answered" "$B" "$relevant" "$rjudged" "$Z"
else
    printf '  answer relevancy: %sskipped%s (set CRAG_RELEVANCY=1%s)\n' \
        "$Y" "$Z" "$([ "$HAVE_JQ" -eq 1 ] && echo "" || echo " and install jq")"
fi
printf '  within-corpus unanswerable (%d): abstained %s%d/%d%s\n' "$nt" "$B" "$nabst" "$nt" "$Z"
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
