#!/bin/sh
# Usage: copy into the keyboard's scripts folder and start it from a Run button; it writes ~/run-child-probe.txt, then sleeps 10 minutes.
#
# What a program the shortcut bar starts gets from the keyboard: environment
# variable names (never their values), its process IDs, session and process
# group, working directory, open descriptors and cgroup. It reads only this
# shell's own /proc entries and writes only the one file in the home folder.
set -u

self=/proc/$$
# Descriptors first, by glob in this shell itself: a command substitution or
# redirection would add one of its own to the list.
set -- "$self"/fd/*
fd_count=0
fd_numbers=
for fd in "$@"; do
    [ -e "$fd" ] || [ -L "$fd" ] || continue
    fd_count=$((fd_count + 1))
    fd_numbers="$fd_numbers ${fd##*/}"
done
working_directory=$(pwd -P)

# Fields after the command name, which may hold spaces: state ppid pgrp session.
stat=$(cat "$self/stat")
set -- ${stat##*) }
ppid=$2
process_group=$3
session=$4

umask 077
output="$HOME/run-child-probe.txt"
{
    echo "pid=$$"
    echo "ppid=$ppid"
    echo "session=$session"
    echo "process_group=$process_group"
    echo "working_directory=$working_directory"
    echo "open_fds=$fd_count"
    echo "fd_numbers=${fd_numbers# }"
    echo "cgroup:"
    sed 's/^/  /' "$self/cgroup"
    # Names only: each NUL-ended entry cut at its first '='.
    echo "environment_names:"
    cut -z -d= -f1 "$self/environ" | tr '\0' '\n' | sort | sed 's/^/  /'
} > "$output"

# exec: the probe stays this one process, so stopping it stops the sleep.
exec sleep 600
