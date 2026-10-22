cmake_minimum_required(VERSION 3.22)
project(BadAppleOverlay LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(
  levilaunchroid
  GIT_REPOSITORY https://github.com/LiteLDev/LeviLaunchroid.git
  GIT_TAG main
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR app/src/main/cpp/preloader
)
FetchContent_MakeAvailable(levilaunchroid)
add_library(bad_apple_overlay SHARED src/BadAppleOverlay.cpp)
target_link_libraries(bad_apple_overlay PRIVATE preloader log)
target_compile_options(bad_apple_overlay PRIVATE -Wall -Wextra)
