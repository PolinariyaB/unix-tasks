#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

ulimit -n 8192 2>/dev/null || true
ulimit -u 1024 2>/dev/null || true

RESULT_FILE="result.txt"
SERVER_LOG="/tmp/brownian-bot.log"
CLIENT_LOG_DIR="/tmp/brownian-bot-client-logs"
SOCKET_NAME="$(tr -d '\r\n' < config)"
SOCKET_PATH="/tmp/${SOCKET_NAME}"
NUMBERS_FILE="numbers.txt"

SERVER_PID=""
LAST_FAILED_CLIENTS=0
LAST_NONEMPTY_ERRS=0

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

cleanup() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "$SOCKET_PATH"
}
trap cleanup EXIT

generate_numbers_file() {
    local tmp_numbers
    tmp_numbers="$(mktemp)"

    awk '
        BEGIN {
            srand();
            sum = 0;
            for (i = 0; i < 999; i++) {
                value = int(rand() * 201) - 100;
                values[i] = value;
                sum += value;
            }
            for (i = 0; i < 999; i++) {
                print values[i];
            }
            print -sum;
        }
    ' >"$tmp_numbers"

    shuf "$tmp_numbers" >"$NUMBERS_FILE"
    rm -f "$tmp_numbers"
}

make clean && make

rm -f "$RESULT_FILE" "$SERVER_LOG"
rm -rf "$CLIENT_LOG_DIR"
mkdir -p "$CLIENT_LOG_DIR"
rm -f "$SOCKET_PATH"
generate_numbers_file

./server &
SERVER_PID="$!"

for _ in $(seq 1 100); do
    if [[ -S "$SOCKET_PATH" ]]; then
        break
    fi
    sleep 0.1
done
if [[ ! -S "$SOCKET_PATH" ]]; then
    echo "Server socket was not created: $SOCKET_PATH" >&2
    exit 1
fi

numbers_sum="$(awk '{ sum += $1 } END { print sum + 0 }' "$NUMBERS_FILE")"
numbers_count="$(wc -l <"$NUMBERS_FILE" | tr -d '[:space:]')"
if [[ "$numbers_sum" == "0" ]]; then
    numbers_result="PASS"
else
    numbers_result="FAIL"
fi
write_block "DATASET: generated numbers.txt" "sum=0" "sum=$numbers_sum lines=$numbers_count file=$NUMBERS_FILE" "$numbers_result"

run_clients_batch() {
    local clients="$1"
    local delay="$2"
    local prefix="$3"
    local i
    local pids=()
    local failed=0
    local err_nonempty=0

    rm -f "$CLIENT_LOG_DIR"/"${prefix}"_*.log "$CLIENT_LOG_DIR"/"${prefix}"_*.err

    for i in $(seq 1 "$clients"); do
        ./test_client "$NUMBERS_FILE" "$delay" "$CLIENT_LOG_DIR/${prefix}_${i}.log" 2>"$CLIENT_LOG_DIR/${prefix}_${i}.err" &
        pids+=("$!")
        sleep 0.02
    done

    for i in "${pids[@]}"; do
        if ! wait "$i"; then
            failed=$((failed + 1))
        fi
    done

    for i in $(seq 1 "$clients"); do
        if [[ -s "$CLIENT_LOG_DIR/${prefix}_${i}.err" ]]; then
            err_nonempty=$((err_nonempty + 1))
        fi
    done

    LAST_FAILED_CLIENTS="$failed"
    LAST_NONEMPTY_ERRS="$err_nonempty"
    return 0
}

wait_server_ready() {
    local max_attempts=100
    local attempt=0

    while [[ "$attempt" -lt "$max_attempts" ]]; do
        if ! server_alive; then
            return 1
        fi

        if [[ -S "$SOCKET_PATH" ]]; then
            if printf '0\n' | timeout 2 ./client 2>/dev/null | grep -q '^0$'; then
                return 0
            fi
        fi
        attempt=$((attempt + 1))
        sleep 0.1
    done

    return 1
}

wait_between_batches() {
    sleep 5
    wait_server_ready || echo "Warning: server not responding after batch sync" >&2
}

probe_state() {
    printf '0\n' | ./client 2>>"$CLIENT_LOG_DIR/probe.err" | awk 'END { gsub(/\r/, "", $0); print $0 }'
}

server_alive() {
    [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null
}

wait_server_ready || {
    echo "Server failed to start" >&2
    exit 1
}


run_clients_batch 100 0.05 "test1"
wait_between_batches
if [ -f "$CLIENT_LOG_DIR/test1_1.err" ]; then
    echo "=== Client 1 error (if any) ===" >> "$RESULT_FILE"
    cat "$CLIENT_LOG_DIR/test1_1.err" >> "$RESULT_FILE" 2>/dev/null || true
fi
actual1="$(probe_state || echo "probe_failed")"
if [[ "$actual1" == "0" && "$LAST_FAILED_CLIENTS" -eq 0 ]]; then
    r1="PASS"
else
    r1="FAIL"
fi
write_block "TEST 1: 100 clients, final state zero" "0 and failed_clients=0" "actual=$actual1 failed_clients=$LAST_FAILED_CLIENTS err_files_nonempty=$LAST_NONEMPTY_ERRS" "$r1"

if server_alive; then
    r1a="PASS"
else
    r1a="FAIL"
fi
write_block "TEST 1A: server alive after batch 1" "server process is alive" "server_alive=$r1a" "$r1a"

run_clients_batch 100 0.05 "test2"
wait_between_batches
actual2="$(probe_state || echo "probe_failed")"
if [[ "$actual2" == "0" && "$LAST_FAILED_CLIENTS" -eq 0 ]]; then
    r2="PASS"
else
    r2="FAIL"
fi
write_block "TEST 2: repeat run without server restart" "0 and failed_clients=0" "actual=$actual2 failed_clients=$LAST_FAILED_CLIENTS err_files_nonempty=$LAST_NONEMPTY_ERRS" "$r2"

if server_alive; then
    r2a="PASS"
else
    r2a="FAIL"
fi
write_block "TEST 2A: server alive after batch 2" "server process is alive" "server_alive=$r2a" "$r2a"

{
    printf 'TEST 3: performance experiments\n'
    printf 'Expected: statistics collected for all client counts and delays\n'
    printf 'Actual:\n'
} >>"$RESULT_FILE"

perf_result="PASS"
for clients in 1 10 50 100; do
    for delay in 0 0.2 0.4 0.6 0.8 1.0; do
        run_clients_batch "$clients" "$delay" "perf_${clients}_${delay}"
        wait_between_batches
        if server_alive; then
            perf_alive="yes"
        else
            perf_alive="no"
        fi

        stats="$(
            awk -F= '
                /^started_at=/ { if (min == "" || $2 + 0 < min) min = $2 + 0 }
                /^finished_at=/ { if (max_finish == "" || $2 + 0 > max_finish) max_finish = $2 + 0 }
                /^total_delay=/ { if ($2 + 0 > max_delay) max_delay = $2 + 0 }
                END {
                    if (min == "") min = 0;
                    if (max_finish == "") max_finish = 0;
                    print min " " max_finish " " max_delay
                }
            ' "$CLIENT_LOG_DIR"/perf_"${clients}"_"${delay}"_*.log
        )"

        first_start="$(echo "$stats" | awk '{print $1}')"
        last_finish="$(echo "$stats" | awk '{print $2}')"
        max_delay="$(echo "$stats" | awk '{print $3}')"

        effective="$(
            awk -v fs="$first_start" -v lf="$last_finish" -v md="$max_delay" '
                BEGIN {
                    e = (lf - fs) - md;
                    if (e < 0) e = 0;
                    printf "%.6f", e
                }
            '
        )"

        if [[ "$LAST_FAILED_CLIENTS" -ne 0 ]]; then
            perf_result="FAIL"
        fi

        printf 'clients=%s delay=%s effective=%s failed_clients=%s err_files_nonempty=%s server_alive=%s\n' \
            "$clients" "$delay" "$effective" "$LAST_FAILED_CLIENTS" "$LAST_NONEMPTY_ERRS" "$perf_alive" >>"$RESULT_FILE"
    done
done

overall="PASS"
if [[ "$numbers_result" != "PASS" || "$r1" != "PASS" || "$r1a" != "PASS" || "$r2" != "PASS" || "$r2a" != "PASS" || "$perf_result" != "PASS" ]]; then
    overall="FAIL"
fi

heap_check_result="SKIP"
heap_first=""
heap_last=""
if [[ -f "$SERVER_LOG" ]]; then
    heap_first="$(grep -m1 'CONNECT fd=.* heap=' "$SERVER_LOG" || true)"
    heap_last="$(grep 'CONNECT fd=.* heap=' "$SERVER_LOG" | tail -n1 || true)"

    if [[ -n "$heap_first" && -n "$heap_last" ]]; then
        first_heap="$(printf '%s\n' "$heap_first" | sed -n 's/.*heap=\(0x[0-9a-fA-F]\+\).*/\1/p')"
        last_heap="$(printf '%s\n' "$heap_last" | sed -n 's/.*heap=\(0x[0-9a-fA-F]\+\).*/\1/p')"
        if [[ -n "$first_heap" && -n "$last_heap" ]]; then
            if [[ "$first_heap" == "$last_heap" ]]; then
                heap_check_result="PASS"
            else
                heap_check_result="FAIL"
                overall="FAIL"
            fi
            write_block "TEST 4: CONNECT heap comparison" "first CONNECT heap equals last CONNECT heap" "first=$first_heap last=$last_heap" "$heap_check_result"
        else
            write_block "TEST 4: CONNECT heap comparison" "parsable heap values in server log" "first_line=${heap_first:-missing} last_line=${heap_last:-missing}" "FAIL"
            heap_check_result="FAIL"
            overall="FAIL"
        fi
    else
        write_block "TEST 4: CONNECT heap comparison" "CONNECT heap lines present in server log" "first_line=${heap_first:-missing} last_line=${heap_last:-missing}" "SKIP"
    fi
else
    write_block "TEST 4: CONNECT heap comparison" "server log exists" "missing $SERVER_LOG" "SKIP"
fi

printf 'Result: %s\n\n' "$overall" >>"$RESULT_FILE"

cat "$RESULT_FILE"
