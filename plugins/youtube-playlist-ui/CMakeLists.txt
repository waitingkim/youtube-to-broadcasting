cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(youtube-playlist-ui MODULE)
add_library(OBS::youtube-playlist-ui ALIAS youtube-playlist-ui)

target_sources(youtube-playlist-ui PRIVATE yt-ui-main.cpp playlist-dock.cpp playlist-dock.hpp)

target_compile_options(
  youtube-playlist-ui
  PRIVATE $<$<PLATFORM_ID:Darwin>:-Wno-quoted-include-in-framework-header> $<$<PLATFORM_ID:Darwin>:-Wno-comma>
)

target_link_libraries(youtube-playlist-ui PRIVATE OBS::frontend-api OBS::libobs Qt::Widgets)

set_target_properties_obs(youtube-playlist-ui PROPERTIES FOLDER plugins PREFIX "" AUTOMOC ON)
