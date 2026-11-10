#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$(realpath "${1:-$source_dir/build/cmake-debug}")"
mode="${2:-named}"
scenario="${3:-all}"
temporary_dir=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT
cd "$temporary_dir"

start() {
    rm -f -- garden.log
    if [[ "$mode" == named || "$mode" == unnamed ]]; then
        "$build_dir/gardeners_$mode" input.txt > output.txt 2> error.txt &
        server=$!; pids=("$server")
    else
        session="test_$$_$RANDOM"
        "$build_dir/garden_server" "$mode" "$session" input.txt > output.txt 2> error.txt &
        server=$!; pids=("$server")
        for ((attempt=0; attempt<200; ++attempt)); do
            grep -q READY output.txt && break
            kill -0 "$server" 2>/dev/null || break
            sleep .01
        done
        grep -q READY output.txt || { cat error.txt; return 1; }
        "$build_dir/gardener" "$mode" "$session" 1 > worker1.txt 2>> error.txt &
        pids+=("$!")
        "$build_dir/gardener" "$mode" "$session" 2 > worker2.txt 2>> error.txt &
        pids+=("$!")
    fi
}
finish() {
    local expected="$1" code=0
    wait "$server" || code=$?
    [[ "$code" -eq "$expected" ]] || { cat output.txt error.txt; echo "Код $code вместо $expected"; exit 1; }
    for pid in "${pids[@]:1}"; do wait "$pid"; done
    pids=()
    # Имена IPC каждого запуска записаны в выводе/журнале; ничего не должно остаться.
    local name="$session"
    [[ "$mode" == named || "$mode" == unnamed ]] && name="fork_$server"
    local number=0
    case "$mode" in unnamed) number=1;; shm) number=2;; mq) number=3;; esac
    local prefix="akos3_$(id -u)_${number}_${name}_"
    [[ -z "$(find /dev/shm /dev/mqueue -maxdepth 1 -name "*$prefix*" -print)" ]]
}
session=unused
for name in demo rows blocked isolated waiting overflow interrupt; do
    [[ "$scenario" == all || "$scenario" == "$name" ]] || continue
    case "$name" in
        demo|rows|blocked|isolated|waiting)
            file="$name"; [[ "$name" == rows ]] && file=empty
            if [[ "$name" == waiting ]]; then
                printf '1 3\n20 20 20\nC 1\n0 0 0 2\n1\n...\n' > input.txt
            else awk 'NR==5 {$0=1} {print}' "$source_dir/data/$file.txt" > input.txt; fi
            start
            expected=0; [[ "$name" == blocked ]] && expected=2
            finish "$expected"
            case "$name" in
                demo) grep -q 'обработано 43 из 43' garden.log;;
                rows) grep -q 'обработано 12 из 12' garden.log;;
                isolated) grep -q 'обработано 2 из 2' garden.log; grep -q '(1,1): недостижима' garden.log;;
                waiting) grep -q 'ожидает путь' garden.log; grep -q 'обработано 3 из 3' garden.log;;
            esac
            [[ "$(grep -c 'освободил клетки' garden.log)" -eq 2 ]]
            ;;
        overflow)
            for value in 4294967297 -4294967295 9223372036854775808; do
                printf '%s 2\n1 2 2\nC 1\n0 0 0 1\n0\n..\n' "$value" > input.txt
                code=0
                if [[ "$mode" == named || "$mode" == unnamed ]]; then
                    "$build_dir/gardeners_$mode" input.txt > output.txt 2> error.txt || code=$?
                else "$build_dir/garden_server" "$mode" "bad_$$" input.txt > output.txt 2> error.txt || code=$?; fi
                [[ "$code" -eq 1 ]]; grep -q 'Некорректные параметры' error.txt
            done
            ;;
        interrupt)
            for signal in INT TERM; do
                printf '1 3\n100 100 1\nC 1\n0 0 0 2\n10\n...\n' > input.txt
                start
                for ((attempt=0; attempt<300; ++attempt)); do
                    grep -q 'ожидает путь' garden.log 2>/dev/null && break
                    sleep .01
                done
                grep -q 'ожидает путь' garden.log
                kill -s "$signal" "$server"
                code=130; [[ "$signal" == TERM ]] && code=143
                finish "$code"
                grep -q 'прервано пользователем' garden.log
            done
            ;;
    esac
    echo "$mode $name: OK"
done
