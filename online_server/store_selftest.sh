#!/bin/bash
# store_selftest.sh - what the server does with a marker store it is started on.
#
#   bash online_server/store_selftest.sh <sh_master> [<shnet_selftest>]
#
# Two hand-made stores. One in the layout from before messages had lifetimes:
# it must be taken in once, every marker's seven days starting at that moment,
# the old ratings split into likes and dislikes, and the file as it was kept
# beside the new one. One in the current layout, with messages on either side
# of the end of their days: seven, and one more for each like. With the
# protocol self-test as a second argument it also leaves a message, likes it,
# restarts the server and checks that the like, and the fact that it was cast,
# are still there. The release workflow runs this before anything is
# published, because a mistake here deletes what players left.
set -euo pipefail

BIN="$(realpath "$1")"
W="$(mktemp -d)"
NOW="$(date +%s)"
DAY=86400
TAB="$(printf '\t')"
SRV=""

cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true; }
trap cleanup EXIT

say() { echo; echo "== $*"; }
fail() { echo "FAILED: $*"; echo "--- log:"; cat "$W/sh_master.log" 2>/dev/null || true; exit 1; }

run() {  # the server on whatever store is there
    rm -f "$W"/sh_master.log*
    ( cd "$W" && exec "$BIN" --no-update-check -p 27961 --admin 27962 -d "$W/memos.db" -i 100 > "$W/out.txt" 2>&1 ) &
    SRV=$!
    for i in $(seq 1 50); do
        grep -q "listening on UDP" "$W/sh_master.log" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "the server did not start"
}

start() {  # start <store to begin from>
    rm -f "$W/memos.db" "$W/memos.db.v1" "$W/memos.db.votes"
    cp "$1" "$W/memos.db"
    run
}

stop() {
    "$BIN" --admin 27962 --cmd save > /dev/null
    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    SRV=""
}

# field <id> <n>: the nth space-separated field of that marker's line in the store
field() { awk -v id="$1" -v n="$2" '$1 == id { print $n }' "$W/memos.db"; }

say "a store from before lifetimes"
{
    printf '# sh_master marker store v1\n'
    printf '# id owner kind map x y z rot rating phraseA wordA phraseB wordB unix name\n'
    printf '11 500 0 5 40960 0 40960 0 3 3 7 65535 65535 %s Old Liked\n'     $((NOW - 40 * DAY))
    printf '12 500 0 5 45056 0 40960 0 -2 4 8 65535 65535 %s Old Disliked\n' $((NOW - 40 * DAY))
    printf '13 501 2 5 49152 0 40960 0 0 65535 65535 65535 65535 %s Rester\n' $((NOW - 40 * DAY))
    printf '14 502 1 5 53248 0 40960 0 0 65535 65535 65535 65535 %s Dead Long Ago\n' $((NOW - 3 * DAY))
} > "$W/v1.db"
start "$W/v1.db"
grep -q "the 7 days of all 4 markers in it start now" "$W/sh_master.log" || fail "the old store was not taken in"
stop
head -1 "$W/memos.db" | grep -q "marker store v2" || fail "the store was not rewritten in the current layout"
head -1 "$W/memos.db.v1" | grep -q "marker store v1" || fail "the old store was not kept beside it"
[ "$(field 11 9)" = 3 ] && [ "$(field 11 10)" = 0 ] || fail "a rating of +3 did not become 3 likes"
[ "$(field 12 9)" = 0 ] && [ "$(field 12 10)" = 2 ] || fail "a rating of -2 did not become 2 dislikes"
LIFE="$(field 11 16)"
[ $((LIFE - NOW)) -ge -5 ] && [ $((LIFE - NOW)) -le 60 ] || fail "a 40 day old message did not start its days now ($((LIFE - NOW)) s off)"
[ "$(field 11 15)" = $((NOW - 40 * DAY)) ] || fail "the day it was left was not kept"
[ -n "$(field 13 1)" ] || fail "a resting place was removed"
[ -z "$(field 14 1)" ] || fail "a three day old death marker was kept"
echo "taken in: likes and dislikes split, days start now, old file kept, old death marker gone"

say "started a second time, the same store is left as it is"
cp "$W/memos.db" "$W/again.db"
start "$W/again.db"
if grep -q "before messages had lifetimes" "$W/sh_master.log"; then fail "a current store was taken in again"; fi
stop
[ "$(field 11 16)" = "$LIFE" ] || fail "a restart moved the start of a message's days"
[ "$(field 11 9)" = 3 ] && [ "$(field 12 10)" = 2 ] || fail "a restart lost likes or dislikes"
echo "likes, dislikes and the start of the days are the same after a restart"

say "messages at the end of their days"
{
    printf '# sh_master marker store v2\n'
    printf '21 600 0 5 40960 0 40960 0 0 0 3 7 65535 65535 %s %s Eight Days No Likes%s\n'  $((NOW - 8 * DAY)) $((NOW - 8 * DAY)) "$TAB"
    printf '22 600 0 5 45056 0 40960 0 2 0 3 7 65535 65535 %s %s Eight Days Two Likes%s\n' $((NOW - 8 * DAY)) $((NOW - 8 * DAY)) "$TAB"
    printf '23 600 0 5 49152 0 40960 0 2 5 3 65535 65535 65535 %s %s Typed Past Nine%sthe old well\n' $((NOW - 10 * DAY)) $((NOW - 10 * DAY)) "$TAB"
    printf '24 600 0 5 53248 0 40960 0 0 9 3 65535 65535 65535 %s %s Six Days Disliked%sa typed word\n' $((NOW - 6 * DAY)) $((NOW - 6 * DAY)) "$TAB"
    printf '25 601 2 5 57344 0 40960 0 0 0 65535 65535 65535 65535 %s %s Rested Long Ago%s\n' $((NOW - 90 * DAY)) $((NOW - 90 * DAY)) "$TAB"
    printf '26 600 0 5 61440 0 40960 0 0 0 3 65535 65535 65535 %s %s No Word%s\n' "$NOW" "$NOW" "$TAB"
} > "$W/v2.db"
start "$W/v2.db"
"$BIN" --admin 27962 --cmd "memos 600"
stop
[ -z "$(field 21 1)" ] || fail "a message 8 days old with no likes was kept"
[ -n "$(field 22 1)" ] || fail "a message 8 days old with 2 likes was removed (it has 9 days)"
[ -z "$(field 23 1)" ] || fail "a message 10 days old with 2 likes was kept (it had 9 days)"
[ -n "$(field 24 1)" ] || fail "dislikes took days off a message"
[ -n "$(field 25 1)" ] || fail "a resting place was removed for its age"
[ -z "$(field 26 1)" ] || fail "a typed-word message with no word in it was kept"
grep -q "$(printf '24 600 .*Six Days Disliked\ta typed word')" "$W/memos.db" || fail "a typed word did not survive a load and a save"
echo "kept and removed by the rule: 7 days, and 1 more per like"

if [ -n "${2:-}" ]; then
    TEST="$(realpath "$2")"
    say "a like, and who cast it, are kept across a restart"
    printf '# sh_master marker store v2\n' > "$W/empty.db"
    start "$W/empty.db"
    "$TEST" --votes 1 127.0.0.1 27961 | tee "$W/votes1.txt"
    ID="$(sed -n 's/^VOTED_ID=//p' "$W/votes1.txt")"
    grep -q "^PASSED" "$W/votes1.txt" && [ -n "$ID" ] || fail "the message could not be left and liked"
    stop
    grep -q " $ID 1\$" "$W/memos.db.votes" || fail "the vote was not written beside the store"
    if grep -q "127.0.0.1" "$W/memos.db.votes"; then fail "the votes file holds an address"; fi
    run
    grep -q "loaded 1 vote(s)" "$W/sh_master.log" || fail "the vote was not read back"
    "$TEST" --votes 2 --memo "$ID" 127.0.0.1 27961 | tee "$W/votes2.txt"
    grep -q "^PASSED" "$W/votes2.txt" || fail "the like or its voter did not survive the restart"
    stop
fi

echo
echo "store self-test passed"
