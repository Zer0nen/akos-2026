#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "$0")/.." && pwd)"
program="$(realpath "${1:-$source_dir/build/cmake-debug/gardeners}")"
scenario="${2:-all}"
temporary_dir="$(mktemp -d)"
child=""
cleanup() {
    if [[ -n "$child" ]]; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    rm -rf -- "$temporary_dir"
}
trap cleanup EXIT

run_case() {
    local name="$1" input="$2" expected_code="$3" expected_text="$4" code=0
    mkdir -p "$temporary_dir/$name"
    # Небольшая реальная задержка сохраняет перекрытие действий двух потоков.
    awk 'NR == 5 { $0 = 1 } { print }' "$input" > "$temporary_dir/$name/input.txt"
    (cd "$temporary_dir/$name"; "$program" input.txt > output.txt 2> error.txt) || code=$?
    [[ "$code" -eq "$expected_code" ]] || {
        cat "$temporary_dir/$name/output.txt" "$temporary_dir/$name/error.txt"
        echo "Ошибка $name: ожидался код $expected_code, получен $code"; exit 1;
    }
    grep -Fq "$expected_text" "$temporary_dir/$name/output.txt"
    grep -Fq "$expected_text" "$temporary_dir/$name/garden.log"
    [[ "$(grep -c 'освободил клетки' "$temporary_dir/$name/garden.log")" -eq 2 ]]
    echo "$name: OK"
}

for name in demo rows blocked isolated two_cells waiting invalid interrupt; do
    [[ "$scenario" == all || "$scenario" == "$name" ]] || continue
    case "$name" in
        demo) run_case demo "$source_dir/data/demo.txt" 0 "обработано 43 из 43" ;;
        rows) run_case rows "$source_dir/data/empty.txt" 0 "обработано 12 из 12" ;;
        blocked) run_case blocked "$source_dir/data/blocked.txt" 2 "невозможно продолжить обход" ;;
        isolated)
            run_case isolated "$source_dir/data/isolated.txt" 0 "обработано 2 из 2"
            grep -Fq 'Необработанная клетка (1,1): недостижима' "$temporary_dir/isolated/garden.log"
            ;;
        two_cells)
            printf '1 2\n1 2 2\nC 1\n0 0 0 1\n1\n..\n' > "$temporary_dir/two.txt"
            run_case two_cells "$temporary_dir/two.txt" 0 "обработано 2 из 2"
            ;;
        waiting)
            printf '1 3\n20 20 20\nC 1\n0 0 0 2\n1\n...\n' > "$temporary_dir/waiting.txt"
            run_case waiting "$temporary_dir/waiting.txt" 0 "обработано 3 из 3"
            grep -Fq 'ожидает путь' "$temporary_dir/waiting/garden.log"
            ;;
        invalid)
            printf '1 2\n1 2 2\nC 1\n0 0 0 0\n1\n..\n' > "$temporary_dir/invalid.txt"
            code=0
            "$program" "$temporary_dir/invalid.txt" > /dev/null 2> "$temporary_dir/error.txt" || code=$?
            [[ "$code" -eq 1 ]]
            grep -Fq 'Некорректные параметры' "$temporary_dir/error.txt"
            echo 'invalid: OK'
            ;;
        interrupt)
            # Длинная обработка позволяет отправить сигнал до её завершения.
            printf '1 2\n1 2 100\nC 1\n0 0 0 1\n100\n..\n' > "$temporary_dir/input.txt"
            for signal in INT TERM; do
                mkdir -p "$temporary_dir/$signal"
                (cd "$temporary_dir/$signal"; exec "$program" ../input.txt > output.txt 2> error.txt) &
                child=$!
                ready=0
                for ((attempt = 0; attempt < 100; ++attempt)); do
                    if grep -q 'начал обработку' "$temporary_dir/$signal/garden.log" 2>/dev/null; then
                        ready=1; break
                    fi
                    sleep 0.02
                done
                [[ "$ready" -eq 1 ]]
                kill -s "$signal" "$child"
                code=0; wait "$child" || code=$?
                child=""
                expected=130; [[ "$signal" == TERM ]] && expected=143
                [[ "$code" -eq "$expected" ]]
                grep -Fq 'прервано пользователем' "$temporary_dir/$signal/garden.log"
                grep -Fq 'обработано 0 из 2' "$temporary_dir/$signal/garden.log"
                [[ "$(grep -c 'освободил клетки' "$temporary_dir/$signal/garden.log")" -eq 2 ]]
                echo "interrupt $signal: OK"
            done
            ;;
    esac
done
