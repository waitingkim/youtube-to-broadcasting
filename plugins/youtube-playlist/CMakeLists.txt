cmake_minimum_required(VERSION 3.28...3.30)

find_package(FFmpeg 6.1 REQUIRED avcodec avdevice avutil avformat swscale)

if(NOT TARGET OBS::media-playback)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/media-playback" "${CMAKE_BINARY_DIR}/shared/media-playback")
endif()

add_library(youtube-playlist MODULE)
add_library(OBS::youtube-playlist ALIAS youtube-playlist)

target_sources(youtube-playlist PRIVATE youtube-playlist.c yt-source.c yt-fetcher.c yt-fetcher.h yt-proc.c yt-proc.h yt-remux.c yt-remux.h yt-oauth.c yt-oauth.h)

target_compile_options(youtube-playlist PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-shorten-64-to-32>)

target_link_libraries(
  youtube-playlist
  PRIVATE OBS::libobs OBS::media-playback FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>
)

set_target_properties_obs(youtube-playlist PROPERTIES FOLDER plugins PREFIX "")
