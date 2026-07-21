cmake_minimum_required(VERSION 3.16)
project(functional_calculation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(functional_calculation
  src/calculation.cpp
  src/config_reader.cpp)
target_include_directories(functional_calculation PUBLIC include)

add_executable(functional-calculation src/main.cpp)
target_link_libraries(functional-calculation PRIVATE functional_calculation)

enable_testing()
add_executable(functional-calculation-test tests/calculation_test.cpp)
target_link_libraries(functional-calculation-test PRIVATE functional_calculation)
add_test(NAME functional-calculation-test COMMAND functional-calculation-test)
