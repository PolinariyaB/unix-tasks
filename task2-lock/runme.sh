#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

RESULT_FILE="result.txt"
STATS_FILE="stats.txt"
LOCK_FILE="shared.txt.lck"
SHARED_FILE="shared.txt"

write_result_block() {
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

touch "$SHARED_FILE"
rm -f "$LOCK_FILE" "$STATS_FILE"

TEST_DURATION=300

pids=()
for _ in $(seq 1 10); do
    ./lockprog "$SHARED_FILE" &
    pids+=("$!")
done

sleep "$TEST_DURATION"

for pid in "${pids[@]}"; do
    kill -INT -- "$pid" 2>/dev/null || true
done

wait || true

test_stats_exists="FAIL"
if [[ -f "$STATS_FILE" ]]; then
    test_stats_exists="PASS"
fi

actual_t1="missing"
if [[ "$test_stats_exists" == "PASS" ]]; then
    actual_t1="file exists"
fi
write_result_block "TEST 1: stats file exists" "file exists" "$actual_t1" "$test_stats_exists"

line_count="0"
if [[ -f "$STATS_FILE" ]]; then
    line_count="$(wc -l <"$STATS_FILE" | tr -d '[:space:]')"
fi

format_ok="PASS"
locks=()
if [[ -f "$STATS_FILE" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
        if [[ "$line" =~ ^PID=[0-9]+[[:space:]]+LOCKS=[0-9]+$ ]]; then
            if [[ "$line" =~ LOCKS=([0-9]+) ]]; then
                locks+=("${BASH_REMATCH[1]}")
            fi
        else
            if [[ -n "${line//[[:space:]]/}" ]]; then
                format_ok="FAIL"
            fi
        fi
    done <"$STATS_FILE"
fi

test_line_count="FAIL"
actual_t2="$line_count lines; ${#locks[@]} well-formed lock summaries"
if [[ "$test_stats_exists" == "PASS" && "$line_count" == "10" && "$format_ok" == "PASS" && "${#locks[@]}" -eq 10 ]]; then
    test_line_count="PASS"
fi
write_result_block "TEST 2: number of processes" "10" "$actual_t2" "$test_line_count"

min_v=""
max_v=""
avg_v=""
fairness_res="FAIL"
if [[ "${#locks[@]}" -gt 0 ]]; then
    min_v="${locks[0]}"
    max_v="${locks[0]}"
    sum_v=0
    for v in "${locks[@]}"; do
        if [[ "$v" -lt "$min_v" ]]; then
            min_v="$v"
        fi
        if [[ "$v" -gt "$max_v" ]]; then
            max_v="$v"
        fi
        sum_v=$((sum_v + v))
    done
    avg_v=$((sum_v / ${#locks[@]}))
    fairness_res="PASS"
    if [[ "$min_v" -eq 0 ]]; then
        if [[ "$max_v" -gt 0 ]]; then
            fairness_res="WARN"
        fi
    else
        if [[ "$max_v" -gt $((min_v * 2)) ]]; then
            fairness_res="WARN"
        fi
    fi
fi

if [[ "$test_line_count" != "PASS" ]]; then
    fairness_res="FAIL"
fi

actual_t3="min=${min_v:-n/a}, max=${max_v:-n/a}, avg=${avg_v:-n/a}"
write_result_block "TEST 3: fairness" "similar lock counts" "$actual_t3" "$fairness_res"

test_lck="FAIL"
actual_t4="present"
if [[ ! -e "$LOCK_FILE" ]]; then
    test_lck="PASS"
    actual_t4="no .lck file"
fi
write_result_block "TEST 4: lock file cleanup" "no .lck file" "$actual_t4" "$test_lck"
