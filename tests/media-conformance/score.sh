#!/usr/bin/env bash
# Media-conformance scoreboard: tally MEDIA: markers from every lane's markers
# file into a fixed-width PASS/FAIL/SKIP table + a failures dump. Stateless over
# the OUTDIR. PASS = the conformance tool/visual oracle confirmed the function;
# SKIP = not applicable on this DTB/model (first-class, not a fail).
set -u
OUTDIR=${1:-/tmp/media-conformance-out}
P=0 F=0 S=0
printf '%-26s %-6s %s\n' "CASE" "RESULT" "DETAIL"
printf '%-26s %-6s %s\n' "----" "------" "------"
fails=""
for mk in "$OUTDIR"/*-markers.txt; do
    [ -e "$mk" ] || continue
    while IFS= read -r line; do
        case "$line" in
          MEDIA:PASS:*) r=PASS; rest=${line#MEDIA:PASS:} ;;
          MEDIA:FAIL:*) r=FAIL; rest=${line#MEDIA:FAIL:} ;;
          MEDIA:SKIP:*) r=SKIP; rest=${line#MEDIA:SKIP:} ;;
          *) continue ;;
        esac
        name=${rest%%:*}; detail=${rest#*:}; [ "$detail" = "$rest" ] && detail=""
        printf '%-26s %-6s %s\n' "$name" "$r" "$detail"
        case "$r" in
          PASS) P=$((P+1)) ;;
          FAIL) F=$((F+1)); fails="$fails$name: $detail\n" ;;
          SKIP) S=$((S+1)) ;;
        esac
    done < "$mk"
done
echo
printf 'TOTAL: %d PASS  %d FAIL  %d SKIP\n' "$P" "$F" "$S"
if [ "$F" -gt 0 ]; then
    echo "--- failures ---"; printf "$fails"
fi
[ "$F" -eq 0 ]
