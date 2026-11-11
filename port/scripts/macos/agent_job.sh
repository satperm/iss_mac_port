#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Long jobs for unattended agent sessions: detached from the agent's shell (they survive the end of a
# turn and a restart of the session), bounded, with their state in files anyone can read later.
#
#   agent_job.sh start <name> <max seconds> <command...>   # returns at once
#   agent_job.sh status [name]                             # running / done (exit code) / overdue, tail of the log
#   agent_job.sh wait <name> [seconds, default 540]        # returns when done or after the wait; never longer
#
# Files: out/agent/jobs/<name>.{log,start,deadline,exit}. One job at a time uses the game: `start`
# refuses while another job is running (the game runs measure nothing side by side).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
dir=out/agent/jobs
mkdir -p "$dir"
running() { # name -> 0 when its process is alive
    [[ -f $dir/$1.pid && ! -f $dir/$1.exit ]] && kill -0 "$(cat "$dir/$1.pid")" 2>/dev/null
}
case ${1:-} in
start)
    name=$2 max=$3; shift 3
    for p in "$dir"/*.pid; do
        [[ -f $p ]] || continue
        other=$(basename "$p" .pid)
        running "$other" && { echo "job '$other' is still running: wait for it" >&2; exit 1; }
    done
    rm -f "$dir/$name".{log,exit,pid,start,deadline}
    date +%s > "$dir/$name.start"
    echo $(( $(date +%s) + max )) > "$dir/$name.deadline"
    printf '%q ' "$@" > "$dir/$name.cmd"
    # perl's alarm: /usr/local/bin/timeout is an x86-64 binary here and would run the job under Rosetta.
    nohup bash -c 'perl -e '"'"'alarm shift; exec @ARGV'"'"' "$0" "$@" > "'"$dir/$name.log"'" 2>&1; echo $? > "'"$dir/$name.exit"'"' \
        "$max" "$@" > /dev/null 2>&1 &
    echo $! > "$dir/$name.pid"
    disown
    echo "started '$name' (limit ${max} s, until $(date -r "$(cat "$dir/$name.deadline")" +%T)); log: $PWD/$dir/$name.log"
    ;;
status)
    for p in "$dir"/${2:-*}.start; do
        [[ -f $p ]] || { echo "no jobs"; exit 0; }
        name=$(basename "$p" .start)
        if [[ -f $dir/$name.exit ]]; then state="done, exit $(cat "$dir/$name.exit")"
        elif running "$name"; then state="running, $(( $(cat "$dir/$name.deadline") - $(date +%s) )) s left of its limit"
        else state="DEAD without an exit code (killed with the session?)"; fi
        echo "$name: $state (started $(date -r "$(cat "$p")" '+%m-%d %T'))"
        [[ -n ${2:-} ]] && tail -${3:-15} "$dir/$name.log" 2>/dev/null
    done
    ;;
wait)
    name=$2 limit=${3:-540}
    for ((t = 0; t < limit; t += 5)); do
        [[ -f $dir/$name.exit ]] && break
        running "$name" || break
        sleep 5
    done
    "$0" status "$name" 8
    ;;
*) sed -n 3,12p "$0"; exit 2 ;;
esac
