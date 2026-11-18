#!/bin/bash
# update_selftest.sh - the server's version and update path, end to end, on Linux.
#
#   bash online_server/update_selftest.sh <sh_master> <folder with its source> <its version number>
#
# A stand-in release is served from this machine over http: the same source
# built as the next version, and a version.txt that names it. The server under
# test is then made to notice it, to refuse a release whose program is not the
# version it claims, to install the good one while it is serving, to restart
# into it, and to follow the answers a person types at its start (through
# `script`, which gives it a terminal to ask on). The release workflow runs
# this before anything is published.
set -euo pipefail

BIN="$(realpath "$1")"
SRC="$(realpath "$2")"
VER="$3"
NEXT=$((VER + 1))
W="$(mktemp -d)"
HTTP=27950
HTTP_PID=""
SRV_PID=""

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
    pkill -f "sh_master -p 2795" 2>/dev/null || true
}
trap cleanup EXIT

say() { echo; echo "== $*"; }
fail() { echo "FAILED: $*"; exit 1; }

# has <text> <command...>: runs the command, shows what it printed, and says
# whether the text is in it. No pipe: a reader that stops at the first match
# would fail the writer under pipefail.
has() {
    local text="$1" out
    shift
    out="$("$@" 2>&1)" || true
    printf '%s\n' "$out"
    case "$out" in *"$text"*) return 0 ;; esac
    return 1
}

# wait_log <file> <text> [times]: until the file holds that many lines with the text
wait_log() {
    local n="${3:-1}" i c
    for i in $(seq 1 100); do
        c="$(grep -c -F -- "$2" "$1" 2>/dev/null || true)"
        [ "${c:-0}" -ge "$n" ] && return 0
        sleep 0.2
    done
    echo "--- $1:"
    cat "$1" 2>/dev/null || true
    fail "\"$2\" did not appear $n time(s) in $1"
}

# host <name>: a folder holding the server the way a host has it
host() {
    local d="$W/$1"
    mkdir -p "$d"
    cp "$BIN" "$d/sh_master"
    printf 'update_check=ask\nupdate_url=http://127.0.0.1:%s\n' "$HTTP" > "$d/sh_master.cfg"
    echo "$d"
}

# ask <folder> <letter>: starts the server by hand on a terminal where that
# letter gets typed, lets it run a few seconds, and shows what the terminal showed
ask() {
    ( cd "$1" && { printf '%s\n' "$2"; sleep 4; } |
        timeout 8 script -qec "./sh_master -p 27953 --admin 27954 -d memos.db" /dev/null > term.txt 2>&1 ) || true
    pkill -f "sh_master -p 27953" 2>/dev/null || true
    sleep 0.3
    cat "$1/term.txt"
}

say "version $VER, and the stand-in release (version $NEXT)"
has "sh_master version $VER" "$BIN" --version || fail "--version does not say version $VER"
"$BIN" --is-version "$VER" || fail "--is-version $VER"
if "$BIN" --is-version "$NEXT"; then fail "--is-version answers to a version it is not"; fi

mkdir -p "$W/release"
gcc -O2 -static -DSRV_VERSION_NUM="$NEXT" -o "$W/good" "$SRC/sh_master.c"
cp "$W/good" "$W/release/sh_master-linux-x64"
printf 'version %s\nproto 1\n' "$VER" > "$W/release/version.txt"
python3 -m http.server "$HTTP" --bind 127.0.0.1 --directory "$W/release" > /dev/null 2>&1 &
HTTP_PID=$!
for i in $(seq 1 50); do
    curl -fs "http://127.0.0.1:$HTTP/version.txt" > /dev/null && break
    sleep 0.1
done

say "nothing newer: no update"
H="$(host a)"
"$H/sh_master" --check-update || fail "--check-update found an update where there is none"

say "a newer release is noticed"
printf 'version %s\ncommit feedfacefeedfacefeedface\nbuilt  2031-01-01 00:00:00 UTC\nproto  1\n' "$NEXT" > "$W/release/version.txt"
RC=0
"$H/sh_master" --check-update || RC=$?
[ "$RC" = 10 ] || fail "--check-update ended with $RC, expected 10"

say "a release whose program is not the version it names is refused"
cp "$BIN" "$W/release/sh_master-linux-x64"
if "$H/sh_master" --update --yes; then fail "installed a program that is not version $NEXT"; fi
"$H/sh_master" --is-version "$VER" || fail "the server was changed by a refused update"
[ ! -e "$H/sh_master.new" ] || fail "the refused download was left behind"
cp "$W/good" "$W/release/sh_master-linux-x64"

say "a serving server reports it, in its log and to the console"
( cd "$H" && exec ./sh_master -p 27951 --admin 27952 -d memos.db > out.txt 2>&1 ) &
SRV_PID=$!
wait_log "$H/sh_master.log" "listening on UDP"
wait_log "$H/sh_master.log" "update available: version $NEXT"
has "update: version $NEXT is available" "$H/sh_master" --admin 27952 --cmd version || fail "version does not show it"
has "update: version $NEXT is available" "$H/sh_master" --admin 27952 --console < <(printf 'quit\n') ||
    fail "the console does not show it when it opens"

say "update off and update ask are kept in the settings file"
"$H/sh_master" --admin 27952 --cmd "update off"
grep -q '^update_check=off$' "$H/sh_master.cfg" || fail "update off was not saved"
grep -q '^update_url=http://127.0.0.1' "$H/sh_master.cfg" || fail "saving lost update_url"
has "checks are off" "$H/sh_master" --admin 27952 --cmd version || fail "version does not say the checks are off"
"$H/sh_master" --admin 27952 --cmd "update ask"
grep -q '^update_check=ask$' "$H/sh_master.cfg" || fail "update ask was not saved"

say "update --yes installs it, and the serving server restarts into it"
"$H/sh_master" --admin 27952 --update --yes
"$H/sh_master" --is-version "$NEXT" || fail "the program on disk is not version $NEXT"
"$H/sh_master.old" --is-version "$VER" || fail "the replaced server was not kept as .old"
wait_log "$H/sh_master.log" "=== sh_master started (version $NEXT,"
wait_log "$H/sh_master.log" "listening on UDP" 2
has "version $NEXT, protocol" "$H/sh_master" --admin 27952 --cmd version || fail "the serving server is not version $NEXT"

say "restart, with nobody on, starts it again at once"
has "restarting now" "$H/sh_master" --admin 27952 --cmd restart || fail "restart did not answer"
wait_log "$H/sh_master.log" "=== sh_master started" 3
wait_log "$H/sh_master.log" "listening on UDP" 3
has "0 player(s)" "$H/sh_master" --admin 27952 --cmd players || fail "the restarted server does not answer"
kill "$SRV_PID" 2>/dev/null || true
SRV_PID=""
echo "--- its log:"
cat "$H/sh_master.log"

say "started by hand: A (never ask again) turns the checks off and the server starts"
H="$(host b)"
ask "$H" a
grep -q "A newer server is available: version $NEXT" "$H/term.txt" || fail "the server did not ask"
grep -q '^update_check=off$' "$H/sh_master.cfg" || fail "never ask again was not saved"
"$H/sh_master" --is-version "$VER" || fail "never ask again changed the program"
grep -q "listening on UDP" "$H/sh_master.log" || fail "the server did not start after the answer"

say "started by hand: N starts the server as it is"
H="$(host c)"
ask "$H" n
grep -q "Not now" "$H/term.txt" || fail "N was not taken as not now"
grep -q '^update_check=ask$' "$H/sh_master.cfg" || fail "N changed the settings"
"$H/sh_master" --is-version "$VER" || fail "N changed the program"
grep -q "listening on UDP" "$H/sh_master.log" || fail "the server did not start after N"

say "started by hand: Y installs the new version and starts it"
H="$(host d)"
ask "$H" y
"$H/sh_master" --is-version "$NEXT" || fail "Y did not install version $NEXT"
grep -q "=== sh_master started (version $NEXT," "$H/sh_master.log" || fail "the new version did not start"
if grep -q "=== sh_master started (version $VER," "$H/sh_master.log"; then fail "the old version served before the update"; fi

say "a server whose checks are off does not ask"
H="$(host e)"
printf 'update_check=off\nupdate_url=http://127.0.0.1:%s\n' "$HTTP" > "$H/sh_master.cfg"
ask "$H" y
if grep -q "A newer server" "$H/term.txt"; then fail "it asked although update_check=off"; fi
"$H/sh_master" --is-version "$VER" || fail "it updated although update_check=off"
grep -q "listening on UDP" "$H/sh_master.log" || fail "the server did not start"

echo
echo "update self-test passed"
