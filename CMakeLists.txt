cmake_minimum_required(VERSION 3.19)
project(cdi_recomp C)

set(CMAKE_C_STANDARD 11)

# executable name comes from game.json ("exe_name")
file(READ ${CMAKE_SOURCE_DIR}/game.json GAME_JSON)
string(JSON GAME_EXE ERROR_VARIABLE json_err GET ${GAME_JSON} exe_name)
if(json_err)
  set(GAME_EXE cdigame)
endif()

# RECOMP_INTERP_ONLY: build without the translated game code (gen/). The
# game's program is then read from the player's disc and run by the 68000
# interpreter. This is what the public release ships, since gen/ is a
# translation of the game's own code. It is on automatically when gen/ has
# not been generated (tools/gen.py needs the disc).
if(EXISTS ${CMAKE_SOURCE_DIR}/gen/recomp_table.c)
  option(RECOMP_INTERP_ONLY "build without the recompiled game code" OFF)
else()
  option(RECOMP_INTERP_ONLY "build without the recompiled game code" ON)
endif()

if(RECOMP_INTERP_ONLY)
  set(GEN_DIR ${CMAKE_SOURCE_DIR}/runtime/interp_only)
  set(GEN_SOURCES ${GEN_DIR}/recomp_stub.c)
else()
  set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)
  file(GLOB GEN_SOURCES ${GEN_DIR}/*.c)
endif()
file(GLOB RT_SOURCES ${CMAKE_SOURCE_DIR}/runtime/*.c)
file(GLOB HD_SOURCES ${CMAKE_SOURCE_DIR}/runtime/hd/*.c)

# the recompiled game code is compiled once and shared by both executables
add_library(gen_objs OBJECT ${GEN_SOURCES})
target_include_directories(gen_objs PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${GEN_DIR})

# <exe_name>: the faithful build.  <exe_name>_hd: AI-upscaled video pack,
# run-ahead and 60 fps video interpolation (runtime/hd).
add_executable(game ${RT_SOURCES} $<TARGET_OBJECTS:gen_objs>)
set_target_properties(game PROPERTIES OUTPUT_NAME ${GAME_EXE})
add_executable(game_hd ${RT_SOURCES} ${HD_SOURCES} $<TARGET_OBJECTS:gen_objs>)
set_target_properties(game_hd PROPERTIES OUTPUT_NAME ${GAME_EXE}_hd)
target_compile_definitions(game_hd PRIVATE RECOMP_HD)

foreach(t game game_hd gen_objs)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${GEN_DIR})
  if(RECOMP_INTERP_ONLY)
    target_compile_definitions(${t} PRIVATE RECOMP_INTERP_ONLY)
  endif()
  if(MSVC)
    target_compile_options(${t} PRIVATE /O2 /W3 /wd4146 /wd4244 /wd4267 /wd4996 /wd4102 /bigobj)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${t} PRIVATE -O2 -Wno-unused-label)
  endif()
endforeach()
foreach(t game game_hd)
  if(MSVC)
    target_link_libraries(${t} PRIVATE winmm gdi32 user32 comdlg32 winhttp)
    set_target_properties(${t} PROPERTIES WIN32_EXECUTABLE OFF)
  endif()
endforeach()
target_link_libraries(game_hd PRIVATE windowscodecs ole32)

# Differential instruction tester (recompiled semantics vs interpreter).
# Generate test/insn_tests.c with: python tools/gen_tests.py
if(EXISTS ${CMAKE_SOURCE_DIR}/test/insn_tests.c)
  add_executable(insn_test test/harness.c test/insn_tests.c runtime/interp.c runtime/alu.c runtime/disc.c)
  target_include_directories(insn_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_definitions(insn_test PRIVATE RECOMP_TEST _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(insn_test PRIVATE /O1 /bigobj /wd4146 /wd4244 /wd4267 /wd4996 /wd4102)
  endif()
endif()

# ALU helper check against an independent reference model (python test/gen_alu_vectors.py)
if(EXISTS ${CMAKE_SOURCE_DIR}/test/alu_vectors.h)
  add_executable(alu_test test/alu_test.c runtime/alu.c)
  target_include_directories(alu_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_SOURCE_DIR}/test)
endif()
