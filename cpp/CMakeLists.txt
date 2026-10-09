cmake_minimum_required(VERSION 3.16)
project(lighthouses CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

add_library(lhcore STATIC src/pyval.cpp src/engine.cpp src/botplayer.cpp src/frames.cpp)
target_include_directories(lhcore PUBLIC src)

add_executable(lighthouses src/main.cpp)
target_link_libraries(lighthouses lhcore)
get_filename_component(LH_VIEWER_PY "${CMAKE_SOURCE_DIR}/../viewer/viewer.py" ABSOLUTE)
target_compile_definitions(lighthouses PRIVATE LH_VIEWER="${LH_VIEWER_PY}")

# Tests. The fixture generators need a CPython 3.10 interpreter; point
# PYTHON310 at one (e.g. `uv python find 3.10`) to enable them.
enable_testing()
add_executable(test_pyset tests/test_pyset.cpp)
add_executable(test_pyval tests/test_pyval.cpp)
target_link_libraries(test_pyval lhcore)
add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine lhcore)
add_test(NAME engine COMMAND test_engine)

set(PYTHON310 "" CACHE FILEPATH "CPython 3.10 used to generate reference fixtures")
if(PYTHON310)
  foreach(seed 1 2 3)
    add_test(NAME pyset_${seed}
      COMMAND sh -c "'${PYTHON310}' '${CMAKE_SOURCE_DIR}/../tools/setorder_fuzz.py' ${seed} 400 > pyset_${seed}.txt && '$<TARGET_FILE:test_pyset>' pyset_${seed}.txt")
    add_test(NAME pyval_${seed}
      COMMAND sh -c "'${PYTHON310}' '${CMAKE_SOURCE_DIR}/../tools/pyval_fuzz.py' ${seed} 20000 > pyval_${seed}.txt && '$<TARGET_FILE:test_pyval>' pyval_${seed}.txt")
  endforeach()
endif()
