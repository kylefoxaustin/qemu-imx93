#!/bin/sh
#
# Shared guest reaper for the i.MX 93 interconnect harnesses.
#
# These harnesses run on a build box shared with other people and other
# emulators. A guest that survives its harness does not fail loudly: it sits
# at 100% of a core, forever, and every measurement anyone else takes on this
# machine from then on is quietly worse. Nobody challenges a disappointing
# number, so the leak is never found by the results - only by looking.
#
# The reaper each of these harnesses used to carry was:
#
#     trap 'kill ${SPID:-} ${CPID:-}' EXIT      # and: timeout "$TMO" qemu ... &
#
# which is wrong twice, and both mistakes point the same way:
#
#  1. Backgrounding through timeout makes $! the TIMEOUT WRAPPER's pid, not
#     QEMU's. So this kills the wrapper and ORPHANS the guest - it does not
#     stop it, it cuts it loose. The corpse outlives the reaper.
#
#  2. Both the trap and a bare "timeout N" use SIGTERM, which a wedged QEMU
#     (spinning vCPU, stuck under -icount) never gets around to servicing.
#     A "timeout 10" still alive after three hours is not a slow test; it is
#     a reaper waiting on a child that will never answer.
#
# So: kill the wrapper's CHILDREN first, with a signal that cannot be declined,
# and only then the wrapper. And give timeout a -k backstop so the deadline
# path is uncatchable too.
#
# Finally, reap_all PRINTS what it killed. A cleanup that reports nothing is
# indistinguishable from a clean box - which is exactly how this went unseen.
#
# Usage:
#     . "$HERE/reap.sh"                 # installs the EXIT/INT/TERM trap
#     timeout -k 5 "$TMO" "$QEMU" ... & reap_track $!
#
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026, Kyle Fox <kylefoxaustin@gmail.com>

REAP_PIDS=""

reap_track() {
    REAP_PIDS="$REAP_PIDS $1"
}

reap_all() {
    _reaped=""
    for _p in $REAP_PIDS; do
        # The guest lives UNDER the timeout wrapper: take it first, or killing
        # the wrapper sets it free. SIGKILL because a wedged QEMU will ignore
        # anything it is allowed to ignore.
        for _c in $(pgrep -P "$_p" 2>/dev/null); do
            if kill -KILL "$_c" 2>/dev/null; then
                _reaped="$_reaped $_c"
            fi
        done
        if kill -KILL "$_p" 2>/dev/null; then
            _reaped="$_reaped $_p"
        fi
    done
    REAP_PIDS=""
    # Print the corpses. "I left nothing behind" must be evidence, not a hope.
    if [ -n "$_reaped" ]; then
        echo "== reaped leftover guests:$_reaped" >&2
    fi
}

trap 'reap_all' EXIT INT TERM HUP
