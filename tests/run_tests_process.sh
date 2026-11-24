#!/usr/bin/env bash
# Сначала соберите проект: cmake -S . -B build && cmake --build build.
set -eo pipefail
cd "$(dirname "$0")/.."

out=$(mktemp -d "${TMPDIR:-/tmp}/crossroad-test.XXXXXX")
parent=
workers=()
cleanup() {
    if [[ -n "$parent" ]]; then
        kill -TERM "$parent" 2>/dev/null || true
        wait "$parent" 2>/dev/null || true
    fi
    for pid in "${workers[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

start_case() {
    local name=$1
    shift
    log="$out/$mode-$name.log"
    session="$out/$mode-$name"
    if [[ "$mode" == shm || "$mode" == fifo ]]; then
        "bin/crossroad-proc-$mode" "$session" "$@" -l "$log" > "$log.stdout" &
        parent=$!
        for ((i=0; i<200; i++)); do
            [[ -f "$session/ready" ]] && break
            kill -0 "$parent"
            sleep 0.02
        done
        [[ -f "$session/ready" ]]
        for role in cars pedestrians controller north east south west; do
            bin/crossroad-worker "$session" "$role" &
            workers+=("$!")
        done
    else
        "bin/crossroad-proc-$mode" "$@" -l "$log" > "$log.stdout" &
        parent=$!
    fi
}

finish_case() {
    wait "$parent"
    parent=
    for pid in "${workers[@]}"; do wait "$pid"; done
    workers=()
    [[ ! -e "$session" ]]
    grep -Fq 'Нарушений инвариантов: 0' "$log"
    grep -Fq "$1" "$log"
    echo "  OK $(basename "$log")"
}

echo "Журналы $out"
modes=(named shm fifo)
if [[ "$(uname -s)" == Linux ]]; then modes+=(unnamed); fi
for mode in "${modes[@]}"; do
    start_case base -q -d 0 -c data/base.cfg
    finish_case 'истёк период наблюдения'
    grep -Fq 'НАЖАЛ КНОПКУ' "$log"
    grep -Fq 'Переходный интервал' "$log"

    start_case lanes -q -d 0 -c data/lanes.cfg
    finish_case 'пропущено заданное число участников'

    start_case overload -q -d 0 -c data/overload.cfg
    finish_case 'ОТКАЗ'

    start_case stress -q -d 0 -c data/stress.cfg
    finish_case 'истёк период наблюдения'

    start_case interrupt -q -d 10 -c data/infinite.cfg
    for ((i=0; i<200; i++)); do
        if grep -Eq 'прибыло|подошёл' "$log" 2>/dev/null; then break; fi
        kill -0 "$parent"
        sleep 0.02
    done
    grep -Eq 'прибыло|подошёл' "$log"
    if [[ "$mode" == shm || "$mode" == fifo ]]; then
        if bin/crossroad-worker "$session" north 2>/dev/null; then
            echo "Повторный процесс получил занятую роль" >&2
            exit 1
        fi
    fi
    kill -INT "$parent"
    finish_case 'прервано пользователем'

    args=()
    if [[ "$mode" == shm || "$mode" == fifo ]]; then args=("$out/$mode-invalid"); fi
    if "bin/crossroad-proc-$mode" "${args[@]}" -q -c data/bad-phase.cfg > /dev/null 2>&1; then
        echo "Запуск с конфликтующей фазой принят" >&2
        exit 1
    fi
    echo "  OK $mode-invalid"
done

# События при одинаковом seed должны совпасть во всех реализациях.
for mode in "${modes[@]:1}"; do
    diff -u <(grep '^\[t=' "$out/named-base.log") <(grep '^\[t=' "$out/$mode-base.log")
done
echo "События базового сценария совпали."
