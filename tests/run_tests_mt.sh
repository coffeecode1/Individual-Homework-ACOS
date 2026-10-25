#!/bin/bash
# Второе ДЗ: сборка двух многопоточных программ и прогон тестов (вариант 39).
#   crossroad-mt    - семафоры + мьютекс
#   crossroad-mt-cv - условные переменные + мьютекс
# Запуск из папки проекта: bash tests/run_tests_mt.sh
# Журналы сохраняются в tests/out-mt/<программа>-<тест>.log

cd "$(dirname "$0")/.." || exit 1      # переходим в корень проекта
rm -rf tests/out-mt
mkdir -p bin tests/out-mt
SRC="src/config.c src/participants.c src/controller.c src/crossroad.c src/view.c
     src/mt/main_mt.c src/mt/agents.c src/mt/workers.c"
cc -Wall -Wextra -pthread -o bin/crossroad-mt $SRC src/mt/gate_sem.c || exit 1
cc -Wall -Wextra -pthread -DGATE_COND -o bin/crossroad-mt-cv $SRC src/mt/gate_cond.c || exit 1

PASS=0
FAIL=0
# check "название" журнал "строка в журнале" - тест пройден, если в журнале
# есть нужная строка и нет нарушений инвариантов
check() {
    if grep -q "$3" "$2" && grep -q "Нарушений инвариантов: 0" "$2"
    then echo "  OK   $1"; PASS=$((PASS+1))
    else echo "  FAIL $1"; FAIL=$((FAIL+1)); fi
}

for PROG in crossroad-mt crossroad-mt-cv; do
    BIN=./bin/$PROG
    L=tests/out-mt/$PROG
    echo "== $PROG"

    # завершение по периоду наблюдения, по числу участников, отказы при перегрузке,
    # нагрузка на сотни потоков
    $BIN -q -d 0 -c data/base.cfg     -l $L-base.log     > /dev/null
    $BIN -q -d 0 -c data/lanes.cfg    -l $L-lanes.log    > /dev/null
    $BIN -q -d 0 -c data/overload.cfg -l $L-overload.log > /dev/null
    $BIN -q -d 0 -c data/stress.cfg   -l $L-stress.log   > /dev/null
    check "1. base: истёк период наблюдения"        $L-base.log     "истёк период наблюдения"
    check "2. lanes: пропущено 120 участников"      $L-lanes.log    "пропущено заданное число участников"
    check "3. overload: отказы при полной очереди"  $L-overload.log "ОТКАЗ"
    check "4. stress: $(grep -o 'максимум [0-9]*' $L-stress.log) потоков-участников" \
                                                    $L-stress.log   "Потоки:"

    # бесконечный режим и остановка по Ctrl+C
    $BIN -q -d 20 -c data/infinite.cfg -l $L-infinite.log > /dev/null &
    sleep 2
    kill -INT $!
    wait $!
    check "5. infinite: остановлена по Ctrl+C"      $L-infinite.log "прервано пользователем"

    # некорректная фаза: программа не запускается
    if ! $BIN -q -c data/bad-phase.cfg > /dev/null 2>&1
    then echo "  OK   6. bad-phase: запуск отклонён"; PASS=$((PASS+1))
    else echo "  FAIL 6. bad-phase"; FAIL=$((FAIL+1)); fi
done

# ThreadSanitizer: гонок данных нет (если компилятор его поддерживает).
# Проверяется версия на условных переменных: логика потоков у программ общая,
# а именованные семафоры macOS (sem_open) ThreadSanitizer не понимает и выдаёт ложные гонки.
if cc -fsanitize=thread -pthread -DGATE_COND -o bin/crossroad-mt-tsan $SRC src/mt/gate_cond.c 2>/dev/null; then
    ./bin/crossroad-mt-tsan -q -d 0 -c data/overload.cfg -l tests/out-mt/tsan.log > /dev/null 2> tests/out-mt/tsan.txt
    if ! grep -q "WARNING: ThreadSanitizer" tests/out-mt/tsan.txt
    then check "7. tsan: гонок данных нет" tests/out-mt/tsan.log "Потоки:"
    else echo "  FAIL 7. tsan: см. tests/out-mt/tsan.txt"; FAIL=$((FAIL+1)); fi
    rm -f bin/crossroad-mt-tsan
fi

echo "Итого: пройдено $PASS, не пройдено $FAIL"
[ $FAIL -eq 0 ]
