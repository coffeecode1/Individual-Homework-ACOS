#!/bin/bash
# Сборка программы и прогон 5 основных тестов (вариант 39).
# Запуск из папки проекта: bash tests/run_tests.sh
# Журнал каждого теста сохраняется в tests/out/<имя>.log

cd "$(dirname "$0")/.." || exit 1      # переходим в корень проекта
rm -rf tests/out
mkdir -p bin tests/out
cc -Wall -Wextra -o bin/crossroad src/*.c || { echo "Ошибка сборки"; exit 1; }

BIN=./bin/crossroad
OUT=tests/out
PASS=0
FAIL=0
ok()   { echo "  OK   $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL $1"; FAIL=$((FAIL+1)); }

# 1. Обычный сценарий: завершение по периоду наблюдения, нарушений инвариантов нет
$BIN -q -d 0 -c data/base.cfg -l $OUT/base.log > /dev/null
if grep -q "истёк период наблюдения" $OUT/base.log && grep -q "Нарушений инвариантов: 0" $OUT/base.log
then ok "1. base: завершилась по периоду наблюдения, инварианты не нарушены"
else fail "1. base"; fi

# 2. Завершение после пропуска заданного числа участников
$BIN -q -d 0 -c data/lanes.cfg -l $OUT/lanes.log > /dev/null
if grep -q "пропущено заданное число участников" $OUT/lanes.log && grep -q "Нарушений инвариантов: 0" $OUT/lanes.log
then ok "2. lanes: завершилась после пропуска 120 участников"
else fail "2. lanes"; fi

# 3. Перегрузка: при полной очереди регистрируется отказ (затор)
$BIN -q -d 0 -c data/overload.cfg -l $OUT/overload.log > /dev/null
if grep -q "ОТКАЗ" $OUT/overload.log && grep -q "Нарушений инвариантов: 0" $OUT/overload.log
then ok "3. overload: отказы зарегистрированы, инварианты не нарушены"
else fail "3. overload"; fi

# 4. Некорректная фаза (конфликтующие направления): программа не запускается
ERR=$($BIN -q -d 0 -c data/bad-phase.cfg 2>&1 > /dev/null)
if [ $? -ne 0 ] && echo "$ERR" | grep -q "Ошибка"
then ok "4. bad-phase: $(echo "$ERR" | head -1)"
else fail "4. bad-phase: ошибка не обнаружена"; fi

# 5. Бесконечный режим и остановка по Ctrl+C (SIGINT)
$BIN -q -d 20 -c data/infinite.cfg -l $OUT/infinite.log > /dev/null &
PID=$!
sleep 2
kill -INT $PID
wait $PID
if grep -q "прервано пользователем" $OUT/infinite.log
then ok "5. infinite: остановлена по Ctrl+C, статистика записана в журнал"
else fail "5. infinite"; fi

echo "Итого: пройдено $PASS, не пройдено $FAIL"
[ $FAIL -eq 0 ]
