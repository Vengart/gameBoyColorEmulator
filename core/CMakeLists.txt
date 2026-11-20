add_library(gbcore Cartridge.cpp Bus.cpp Cpu.cpp Ppu.cpp Apu.cpp)
target_include_directories(gbcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(gbcore PRIVATE /W4 /utf-8)  # исходники в UTF-8: строки (типы картриджей) попадают в интерфейс
else()
  target_compile_options(gbcore PRIVATE -Wall -Wextra)
endif()
