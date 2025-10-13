cmake_minimum_required(VERSION 3.10)

# Project name and version
project(3D_OSC VERSION 1.0)

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Add libraries
add_library(tinyosc tinyosc.c tinyosc.h)

# Add compiler warnings
add_compile_options(-Wall -Wextra)

# Add the executable
add_executable(3D_OSC main.c)

# Link math library (needed for atof)
target_link_libraries(3D_OSC m tinyosc)
