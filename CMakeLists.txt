cmake_minimum_required(VERSION 3.16)
project(crossroad C)
set(CMAKE_C_STANDARD 17)

# Исполняемый файл кладём в папку bin в корне проекта
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_executable(crossroad
    src/main.c
    src/config.c
    src/participants.c
    src/controller.c
    src/crossroad.c
    src/view.c
)
target_compile_options(crossroad PRIVATE -Wall -Wextra)

# Второе ДЗ: многопоточная версия. Модули первого ДЗ используются без изменений,
# своя только папка src/mt. Две программы с одинаковой логикой:
#   crossroad-mt    - синхронизация семафорами и мьютексом;
#   crossroad-mt-cv - вместо семафоров условные переменные (доп. требование 8).
find_package(Threads REQUIRED)
set(CROSSROAD_COMMON
    src/config.c
    src/participants.c
    src/controller.c
    src/crossroad.c
    src/view.c
    src/mt/main_mt.c
    src/mt/agents.c
    src/mt/workers.c
)
add_executable(crossroad-mt ${CROSSROAD_COMMON} src/mt/gate_sem.c)
target_compile_options(crossroad-mt PRIVATE -Wall -Wextra)
target_link_libraries(crossroad-mt PRIVATE Threads::Threads)

add_executable(crossroad-mt-cv ${CROSSROAD_COMMON} src/mt/gate_cond.c)
target_compile_definitions(crossroad-mt-cv PRIVATE GATE_COND)
target_compile_options(crossroad-mt-cv PRIVATE -Wall -Wextra)
target_link_libraries(crossroad-mt-cv PRIVATE Threads::Threads)
