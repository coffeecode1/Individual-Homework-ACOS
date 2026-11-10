cmake_minimum_required(VERSION 3.16)
project(crossroad C)
set(CMAKE_C_STANDARD 17)

# Исполняемый файл кладём в папку bin в корне проекта
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

set(CROSSROAD_MODEL
    src/config.c
    src/participants.c
    src/controller.c
    src/crossroad.c
    src/view.c
)
add_executable(crossroad src/main.c ${CROSSROAD_MODEL})
target_compile_options(crossroad PRIVATE -Wall -Wextra)

# Второе ДЗ: многопоточная версия. Модули первого ДЗ используются без изменений,
# своя только папка src/mt. Две программы с одинаковой логикой:
#   crossroad-mt    - синхронизация семафорами и мьютексом;
#   crossroad-mt-cv - вместо семафоров условные переменные (доп. требование 8).
find_package(Threads REQUIRED)
set(CROSSROAD_COMMON
    ${CROSSROAD_MODEL}
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

# Третье ДЗ: один сценарий и четыре способа обмена между процессами.
function(add_crossroad_process target definition)
    add_executable(${target} ${CROSSROAD_MODEL} src/process/ipc.c src/process/worker.c ${ARGN})
    target_compile_definitions(${target} PRIVATE ${definition})
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${target} PRIVATE rt)
    endif()
endfunction()

set(PROCESS_MAIN src/process/main_process.c)
add_crossroad_process(crossroad-proc-named PROCESS_MODE=FORK_NAMED ${PROCESS_MAIN})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_crossroad_process(crossroad-proc-unnamed PROCESS_MODE=FORK_UNNAMED ${PROCESS_MAIN})
endif()
add_crossroad_process(crossroad-proc-shm PROCESS_MODE=INDEPENDENT_SHM ${PROCESS_MAIN})
add_crossroad_process(crossroad-proc-fifo PROCESS_MODE=INDEPENDENT_FIFO ${PROCESS_MAIN})
add_crossroad_process(crossroad-worker PROCESS_WORKER)
