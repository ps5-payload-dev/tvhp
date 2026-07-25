cmake_minimum_required(VERSION 3.16)

project(tvhp)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)
find_package(RmlUi REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
			 libavcodec libavformat libavutil libswresample libswscale)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)

add_executable(tvhp
    src/main.cpp
    src/app.cpp
    src/app_connect.cpp
    src/app_guide.cpp
    src/app_recordings.cpp
    src/app_watch.cpp
    src/config.cpp
    src/format.cpp
    src/htsp.cpp
    src/player.cpp
    src/RmlUi/Backend_SDL_Renderer.cpp
    src/RmlUi/Renderer_SDL.cpp
    src/RmlUi/Platform_SDL.cpp
)

add_library(hts STATIC
  libhts/htsmsg.c
  libhts/htsmsg_binary.c
  libhts/htsbuf.c
  libhts/sha1.c
  libhts/net_posix.c
)
target_include_directories(hts PUBLIC libhts)

target_link_libraries(tvhp PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    RmlUi::Core
    PkgConfig::FREETYPE2
    PkgConfig::FFMPEG
    PkgConfig::SDL2_IMAGE
    hts
)
