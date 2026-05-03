#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"
RESULT_FILE="result.txt"
WORKDIR="/tmp/myinit-test-$$"
LOG_FILE="/tmp/myinit.log"
WORKER="$SCRIPT_DIR/test_worker"
CONFIG_PATH="$WORKDIR/config.txt"
export MYINIT_PIDFILE="$WORKDIR/myinit.pid"

write_block() {
    local title="$1"
    local expected="$2"
    local actual="$3"
    local result="$4"

    {
        printf '%s\n' "$title"
        printf 'Expected: %s\n' "$expected"
        printf 'Actual: %s\n' "$actual"
        printf 'Result: %s\n' "$result"
        printf '\n'
    } >>"$RESULT_FILE"
}

rm -f "$RESULT_FILE"

make clean && make

rm -f "$LOG_FILE"
mkdir -p "$WORKDIR"
touch "$WORKDIR/in1" "$WORKDIR/in2" "$WORKDIR/in3"
touch "$WORKDIR/out1" "$WORKDIR/out2" "$WORKDIR/out3"

sed "s|__WORKER__|$WORKER|g; s|__WORKDIR__|$WORKDIR|g" "$SCRIPT_DIR/config_initial.txt" >"$CONFIG_PATH"

./myinit "$CONFIG_PATH"
sleep 1

MYINIT_PID="$(cat "$MYINIT_PIDFILE")"

count_test_children() {
    ps -eo pid=,ppid=,args= 2>/dev/null | awk -v ppid="$MYINIT_PID" '
        $2 == ppid && $0 ~ /test_worker/ { count++ }
        END { print count + 0 }
    '
}

count_test_children_by_name() {
    local name="$1"
    ps -eo pid=,ppid=,args= 2>/dev/null | awk -v ppid="$MYINIT_PID" -v name="$name" '
        $2 == ppid && $0 ~ /test_worker/ && index($0, name) > 0 { count++ }
        END { print count + 0 }
    '
}

find_test_child_pid_by_name() {
    local name="$1"
    ps -eo pid=,ppid=,args= 2>/dev/null | awk -v ppid="$MYINIT_PID" -v name="$name" '
        $2 == ppid && $0 ~ /test_worker/ && index($0, name) > 0 { print $1; exit }
    '
}

c1="$(count_test_children)"
test1="FAIL"
if [[ "$c1" == "3" ]]; then
    test1="PASS"
fi
write_block "TEST 1: initial children count" "3 test_worker children" "$c1" "$test1"

proc2_pid="$(find_test_child_pid_by_name "proc2" || true)"
if [[ -n "$proc2_pid" ]]; then
    pkill -TERM -f "^${WORKER//\//\\/}[[:space:]]+proc2$" 2>/dev/null || true
    if ps -eo pid=,ppid=,args= 2>/dev/null | awk -v pid="$proc2_pid" -v ppid="$MYINIT_PID" '
        $1 == pid && $2 == ppid && $0 ~ /test_worker/ && index($0, "proc2") > 0 { found=1 }
        END { exit found ? 0 : 1 }
    '; then
        kill -TERM "$proc2_pid" 2>/dev/null || true
    fi
fi
sleep 1

c2="$(count_test_children)"
test2="FAIL"
if [[ "$c2" == "3" ]]; then
    test2="PASS"
fi
write_block "TEST 2: restart after child kill" "3 test_worker children" "$c2" "$test2"

sed "s|__WORKER__|$WORKER|g; s|__WORKDIR__|$WORKDIR|g" "$SCRIPT_DIR/config_reloaded.txt" >"$CONFIG_PATH"
kill -HUP "$MYINIT_PID"
sleep 1

c3="$(count_test_children)"
only="$(count_test_children_by_name "onlyone")"
test3="FAIL"
if [[ "$c3" == "1" && "$only" -ge 1 ]]; then
    test3="PASS"
fi
write_block "TEST 3: reload to one child" "1 child with onlyone" "count=$c3 onlyone_lines=$only" "$test3"

st1="$(grep -cE 'START idx=0 .*proc1' "$LOG_FILE" 2>/dev/null; true)"
st2="$(grep -cE 'START idx=1 .*proc2' "$LOG_FILE" 2>/dev/null; true)"
st3="$(grep -cE 'START idx=2 .*proc3' "$LOG_FILE" 2>/dev/null; true)"
restart="$(grep -cE 'RESTART idx=1 ' "$LOG_FILE" 2>/dev/null; true)"
reload="$(grep -c 'RELOAD requested' "$LOG_FILE" 2>/dev/null; true)"
stop="$(grep -cE '^STOP ' "$LOG_FILE" 2>/dev/null; true)"
start_one="$(grep -cE 'START idx=0 .*onlyone' "$LOG_FILE" 2>/dev/null; true)"

test4="FAIL"
if [[ "$st1" -ge 1 && "$st2" -ge 1 && "$st3" -ge 1 && "$restart" -ge 1 && "$reload" -ge 1 && "$stop" -ge 3 && "$start_one" -ge 1 ]]; then
    test4="PASS"
fi
write_block "TEST 4: log contains expected events" "START idx 0-2, RESTART idx=1, RELOAD, 3x STOP, START onlyone" "START0=$st1 START1=$st2 START2=$st3 RESTART=$restart RELOAD=$reload STOP=$stop START_only=$start_one" "$test4"

kill -TERM "$MYINIT_PID" 2>/dev/null || true
sleep 1
for p in $(pgrep -P "$MYINIT_PID" 2>/dev/null || true); do
    kill -KILL "$p" 2>/dev/null || true
done

cat "$RESULT_FILE"
