cmake_minimum_required(VERSION 3.16)

# Рантайм C/C++ линкуется статически (/MT), чтобы exe не зависел от установленного Visual C++ Redistributable.
# Политика CMP0091 включается и для подпроектов (SDL2 объявляет более старую версию CMake).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(gameboyColorEmul LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(core)
add_subdirectory(app)

enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
