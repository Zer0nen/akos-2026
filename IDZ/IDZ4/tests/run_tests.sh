#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/.." && pwd)
bin=$(realpath "$1")
scenario=$2
tmp=$(mktemp -d)
declare -A running=()
cleanup() {
    for pid in "${!running[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${!running[@]}"; do wait "$pid" 2>/dev/null || true; done
    if [[ ${1:-0} != 0 ]]; then
        for file in ./*.out garden.log; do
            [[ ! -f "$file" ]] || { printf '\n%s\n' "$file"; tail -n 25 "$file"; }
        done
    fi
    for file in ./*.out garden.log input.txt; do rm -f -- "$file"; done
    cd /tmp
    rmdir "$tmp"
}
trap 'cleanup "$?"' EXIT
cd "$tmp"

await_text() {
    for ((i=0; i<500; i++)); do
        if grep -Fq -- "$2" "$1" 2>/dev/null; then return; fi
        sleep .01
    done
    printf 'Не дождались сообщения «%s» в %s\n' "$2" "$1" >&2
    return 1
}

start_server() {
    ip=${2:-127.0.0.1}
    "$bin/garden4_server" "$ip" 0 "$1" input.txt >server.out 2>&1 &
    server=$!
    running[$server]=1
    await_text server.out READY
    port=$(awk '/^READY / {print $3}' server.out)
}

worker() {
    "$bin/garden4_worker" "$ip" "$port" "$1" >"$2.out" 2>&1 &
    pid=$!
    running[$pid]=1
}

viewer() {
    "$bin/garden4_view" "$ip" "$port" >"$1.out" 2>&1 &
    pid=$!
    running[$pid]=1
}

expect() {
    local code=0
    wait "$1" || code=$?
    unset 'running[$1]'
    [[ "$code" == "$2" ]] || { printf 'PID %s: код %d вместо %d\n' "$1" "$code" "$2"; return 1; }
}

finish() {
    local code=$1
    shift
    expect "$server" "$code"
    for child in "$@"; do expect "$child" "$code"; done
}

small_map() {
    awk 'NR==5 {$0=20} {print}' "$source_dir/data/$1.txt" >input.txt
}

case "$scenario" in
    basic)
        small_map empty
        start_server basic
        viewer rejected; expect "$pid" 1
        grep -Fq 'Наблюдение выключено' rejected.out
        worker 1 first; a=$pid
        worker 2 second; b=$pid
        finish 0 "$a" "$b"
        grep -Fq 'Садовник 1: начал обработку' first.out
        grep -Fq 'Садовник 2: начал обработку' second.out
        if grep -Eq 'Садовник 2:|Обработано|^[.#12+] ' first.out ||
           grep -Eq 'Садовник 1:|Обработано|^[.#12+] ' second.out ||
           grep -Eq 'Обработано|^[.#12+] ' server.out; then
            printf 'Появился общий вывод в режиме basic.\n' >&2
            exit 1
        fi
        [[ ! -e garden.log ]]
        ;;
    demo|rows|isolated|blocked|yield|corridor)
        case "$scenario" in
            demo) small_map demo; total='43 из 43'; code=0 ;;
            rows) small_map empty; total='12 из 12'; code=0 ;;
            isolated) small_map isolated; total='2 из 2'; code=0 ;;
            blocked) small_map blocked; code=2 ;;
            yield) small_map yield; total='10 из 10'; code=0 ;;
            corridor) small_map corridor; total='2 из 4'; code=2 ;;
        esac
        start_server watch 127.0.0.2
        viewer view; v=$pid
        await_text view.out 'нет связи'
        worker 1 first; a=$pid
        worker 2 second; b=$pid
        finish "$code" "$a" "$b" "$v"
        if [[ "$scenario" == blocked ]]; then
            grep -Eq '^Итог: обработано [0-4] из 5 ' garden.log
            grep -Fq 'обход не завершён' view.out
        else
            grep -Fq "Итог: обработано $total " garden.log
            grep -Fq "Обработано $total" view.out
        fi
        if [[ "$scenario" == isolated ]]; then
            grep -Fq '(1,1): недостижима из обоих стартов' view.out
        fi
        if [[ "$scenario" == yield ]]; then
            grep -Fq 'уступает дорогу' view.out
            [[ $(grep -Fc 'закончил обработку' garden.log) == 10 ]]
        elif [[ "$scenario" == corridor ]]; then
            grep -Fq 'Необработанная клетка (0,0): обход не завершён' view.out
            grep -Fq 'Необработанная клетка (0,3): обход не завершён' view.out
        fi
        ;;
    viewers)
        printf '1 4\n1 1 20\nC 1\n0 0 0 3\n50\n....\n' >input.txt
        start_server watch
        viewer left; v1=$pid
        viewer middle; v2=$pid
        await_text left.out 'нет связи'
        await_text middle.out 'нет связи'
        worker 1 first; a=$pid
        worker 2 second; b=$pid
        await_text left.out 'начал обработку'
        kill -INT "$v1"; expect "$v1" 130
        viewer returned; v3=$pid
        viewer late; v4=$pid
        await_text returned.out 'обрабатывает'
        await_text late.out 'обрабатывает'
        finish 0 "$a" "$b" "$v2" "$v3" "$v4"
        for file in middle returned late; do grep -Fq 'Обработано 4 из 4' "$file.out"; done
        ;;
    yield_reconnect)
        # Уступающий движется достаточно долго, чтобы отключиться до подтверждения перехода.
        awk 'NR==2 {$0="1 40 1"} NR==5 {$0=10} {print}' "$source_dir/data/yield.txt" >input.txt
        start_server watch
        viewer view; v=$pid
        await_text view.out 'нет связи'
        worker 1 first; a=$pid
        worker 2 second; b=$pid
        await_text second.out 'уступает дорогу садовнику 1'
        kill -KILL "$b"; expect "$b" 137
        await_text garden.log 'Садовник 2: отключился, прогресс сохранён'
        worker 2 resumed; b=$pid
        finish 0 "$a" "$b" "$v"
        grep -Fq 'переподключился и продолжает' resumed.out
        [[ $(grep -Fc 'уступает дорогу садовнику 1' garden.log) -ge 2 ]]
        grep -Fq 'Итог: обработано 10 из 10' garden.log
        ;;
    reconnect|worker_move)
        if [[ "$scenario" == reconnect ]]; then
            printf '1 7\n1 1 40\nC 1\n0 0 0 6\n30\n.......\n' >input.txt
            action='Садовник 1: начал обработку (0,1)'
            completed='Садовник 1: закончил обработку (0,1)'
            total='7 из 7'
        else
            printf '1 6\n50 60 1\nC 1\n0 0 0 5\n15\n......\n' >input.txt
            action='Садовник 1: начал переход (0,0) -> (0,1)'
            completed='Садовник 1: закончил переход в (0,1)'
            total='6 из 6'
        fi
        start_server watch
        viewer view; v=$pid
        await_text view.out 'нет связи'
        worker 1 first; a=$pid
        worker 2 second; b=$pid
        await_text garden.log "$action"
        kill -KILL "$a"; expect "$a" 137
        await_text garden.log 'Садовник 1: отключился, прогресс сохранён'
        if [[ "$scenario" == reconnect ]]; then
            await_text garden.log 'Садовник 2: закончил обработку (0,5)'
        fi
        worker 1 resumed; a=$pid
        finish 0 "$a" "$b" "$v"
        grep -Fq 'переподключился и продолжает' resumed.out
        [[ $(grep -Fc -- "$action" garden.log) == 2 ]]
        [[ $(grep -Fc -- "$completed" garden.log) == 1 ]]
        [[ $(grep -Fc 'закончил обработку (0,0)' garden.log) == 1 ]]
        grep -Fq "Обработано $total" view.out
        ;;
    shutdown)
        for sig in INT TERM; do
            printf '30 30\n100 100 100\nC 1\n0 0 29 29\n1000\n' >input.txt
            for ((i=0; i<30; ++i)); do printf '%030d\n' 0 | tr 0 . >>input.txt; done
            start_server watch
            viewer view; v=$pid
            viewer other; v2=$pid
            await_text view.out 'нет связи'
            await_text other.out 'нет связи'
            worker 1 first; a=$pid
            worker 2 second; b=$pid
            await_text first.out 'начал обработку'
            await_text second.out 'начал обработку'
            kill -s "$sig" "$server"
            code=130; [[ "$sig" == INT ]] || code=143
            finish "$code" "$a" "$b" "$v" "$v2"
            for file in first second view other; do
                grep -Fq "Сервер завершил сеанс: сеанс прерван (код $code)." "$file.out"
            done
            grep -Fq 'Необработанная клетка (29,29)' view.out
        done
        ;;
    invalid)
        printf '4294967297 2\n1 1 1\nC 1\n0 0 0 1\n0\n..\n' >input.txt
        code=0
        "$bin/garden4_server" 127.0.0.1 0 watch input.txt >bad.out 2>&1 || code=$?
        [[ "$code" == 1 ]]
        grep -Fq 'Некорректные параметры' bad.out
        small_map empty
        start_server watch
        worker 1 first; a=$pid
        await_text first.out 'работа начнётся после подключения обоих'
        worker 1 duplicate; expect "$pid" 1
        grep -Fq 'уже подключён' duplicate.out
        # Одна строка протокола приходит двумя отдельными записями.
        exec 3<>"/dev/tcp/$ip/$port"
        printf 'WA' >&3
        sleep .05
        printf 'TCH\n' >&3
        IFS= read -r -t 3 frame <&3
        [[ "$frame" == 'VIEW '* ]]
        exec 3>&-
        worker 2 second; b=$pid
        finish 0 "$a" "$b"
        ;;
esac
printf '%s: OK\n' "$scenario"
