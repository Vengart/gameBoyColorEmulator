set(APP_NAME gameBoyColorEmulator)

# SDL2 скачивается и собирается вместе с проектом как статическая библиотека:
# в итоге получается один exe без SDL2.dll рядом.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.zip)
FetchContent_MakeAvailable(SDL2)

add_executable(${APP_NAME}
  main.cpp
  Audio.cpp
  Platform.cpp
  RomLibrary.cpp
  Settings.cpp
  Ui.cpp)
target_link_libraries(${APP_NAME} PRIVATE gbcore SDL2::SDL2-static)
set_target_properties(${APP_NAME} PROPERTIES OUTPUT_NAME GBCEmu)

if(WIN32)
  # Оконное приложение без консоли, но с обычной функцией main()
  set_target_properties(${APP_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_libraries(${APP_NAME} PRIVATE gdi32 ole32 shell32 uuid)
  if(MSVC)
    target_link_options(${APP_NAME} PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

if(MSVC)
  target_compile_options(${APP_NAME} PRIVATE /utf-8 /W4)
  target_compile_definitions(${APP_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
