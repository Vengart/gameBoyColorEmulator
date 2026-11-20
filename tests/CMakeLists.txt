# Тесты без графики и без внешних ROM.
# Тесты создают несколько объектов Bus (по ~250 КБ: VRAM, WRAM, два кадра) на стеке,
# а в отладочной сборке MSVC не переиспользует место под них: даём стек побольше.
if(MSVC)
  add_link_options(/STACK:16777216)
endif()

add_executable(timing_test timing_test.cpp)
target_link_libraries(timing_test PRIVATE gbcore)
add_test(NAME timing COMMAND timing_test ${CMAKE_CURRENT_BINARY_DIR}/timing_test.gb)

add_executable(ppu_test ppu_test.cpp)
target_link_libraries(ppu_test PRIVATE gbcore)
add_test(NAME ppu COMMAND ppu_test)

# Запуск тестового ROM из командной строки: run_rom <файл.gb>; результат печатается из последовательного порта
add_executable(run_rom run_rom.cpp)
target_link_libraries(run_rom PRIVATE gbcore)

# Проверка MBC1, MBC3 (включая часы) и MBC5 на синтетических ROM
add_executable(mbc_test mbc_test.cpp)
target_link_libraries(mbc_test PRIVATE gbcore)
add_test(NAME mbc COMMAND mbc_test)

# HDMA, блокировка VRAM/OAM и тайминг OAM DMA
add_executable(dma_test dma_test.cpp)
target_link_libraries(dma_test PRIVATE gbcore)
add_test(NAME dma COMMAND dma_test)

# Звук: регистры, длительность, свип, питание и генерация отсчётов
add_executable(apu_test apu_test.cpp)
target_link_libraries(apu_test PRIVATE gbcore)
add_test(NAME apu COMMAND apu_test)
