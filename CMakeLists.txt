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
