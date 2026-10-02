cmake_minimum_required(VERSION 3.16)
project(pulse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable strong warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Find Curses with wide character support
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

# Executable target
add_executable(pulse
    src/main.cpp
    src/proc_reader.cpp
    src/renderer.cpp
)

target_include_directories(pulse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pulse PRIVATE ${CURSES_LIBRARIES})
