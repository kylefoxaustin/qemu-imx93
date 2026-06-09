#!/usr/bin/env bash
#
# Hourly health-check / auto-healer for the soak36 supervisor.
# - Reports the supervisor's running state, progress and any incidents.
# - If the supervisor process has died (but the deadline hasn't passed), or it
#   is alive but its heartbeat is stale (wedged), it kills any stray boot and
#   relaunches the supervisor DETACHED against the SAME persisted deadline.
# Prints a one-line verdict (and appends it to $WORK/watch-report.log).
set -u

REPO=${REPO:-/home/kyle/Documents/GitHub/93emulator}
WORK=${WORK:-/tmp/soak36}
STALE=${STALE:-600}          # heartbeat older than this => wedged

now=$(date +%s)
deadline=$(cat "$WORK/deadline" 2>/dev/null || echo 0)
pid=$(cat "$WORK/supervisor.pid" 2>/dev/null || echo "")
hb=$(cat "$WORK/heartbeat" 2>/dev/null || echo 0)
alive=no; [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && alive=yes
hb_age=$(( now - hb ))
remain=$(( deadline - now ))

stat() { grep -E "^$1=" "$WORK/status" 2>/dev/null | cut -d= -f2-; }
inc=$(stat incidents); cyc=$(stat cycles); boots=$(stat boots); phase=$(stat phase)
aud=$(stat audio_plays); cap=$(stat captures); rt=$(stat roundtrips)

relaunch() {
    pkill -f "imx93-11x11-evk.*$WORK" 2>/dev/null
    rm -f "$WORK/supervisor.pid"
    setsid nohup bash "$REPO/tests/soak/soak36.sh" >> "$WORK/soak36.out" 2>&1 < /dev/null &
    sleep 2
}

verdict=""
if [ "$deadline" -eq 0 ]; then
    verdict="NO-RUN (no deadline file; soak not started or already finished)"
elif [ "$now" -ge "$deadline" ]; then
    verdict="DONE (deadline passed; cycles=$cyc boots=$boots incidents=$inc)"
elif [ "$alive" = no ]; then
    verdict="DEAD -> RELAUNCHED (was at cycle=$cyc phase=$phase incidents=$inc)"; relaunch
elif [ "$hb_age" -gt "$STALE" ]; then
    verdict="STUCK hb_age=${hb_age}s -> KILLED+RELAUNCHED (cycle=$cyc phase=$phase)"; relaunch
else
    verdict="HEALTHY pid=$pid phase=$phase cycle=$cyc boots=$boots hb_age=${hb_age}s remain=$(( remain/60 ))min incidents=$inc audio=$aud cap=$cap rt=$rt"
fi

line="[$(date '+%Y-%m-%d %H:%M:%S')] $verdict"
echo "$line"
echo "$line" >> "$WORK/watch-report.log"
