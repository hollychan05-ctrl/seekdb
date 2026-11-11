cmake_minimum_required(VERSION 3.16)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(minivectordb main.cpp sql.cpp db.cpp)
target_compile_options(minivectordb PRIVATE -O2 -Wall -Wextra)
