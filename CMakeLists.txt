cmake_minimum_required(VERSION 3.30)
project(audiorw)
find_package(ads REQUIRED)
find_package(Boost REQUIRED COMPONENTS headers CONFIG)
find_package(miniaudio REQUIRED)
find_package(wavpack REQUIRED)
add_library(audiorw)
add_library(audiorw::audiorw ALIAS audiorw)
target_sources(audiorw PUBLIC
	FILE_SET HEADERS
	BASE_DIRS
		include/audiorw
	FILES
		include/audiorw/audiorw.hpp
)
target_sources(audiorw PRIVATE
	src/audiorw.cpp
)
target_link_libraries(audiorw PUBLIC
	ads::ads
	Boost::headers
	WavPack::WavPack
)
target_link_libraries(audiorw PRIVATE
	$<BUILD_INTERFACE:miniaudio::miniaudio>
)
target_compile_definitions(audiorw PRIVATE
	# MA_NO_AAUDIO
	# MA_NO_ALSA
	# MA_NO_AUDIO4
	# MA_NO_COREAUDIO
	# MA_NO_DEVICE_IO
	# MA_NO_DSOUND
	# MA_NO_GENERATION
	# MA_NO_JACK
	# MA_NO_NULL
	# MA_NO_OPENSL
	# MA_NO_OSS
	# MA_NO_PULSEAUDIO
	# MA_NO_SNDIO
	# MA_NO_THREADING
	# MA_NO_WASAPI
	# MA_NO_WEBAUDIO
	# MA_NO_WINMM
)
include(CMakePackageConfigHelpers)
install(TARGETS audiorw EXPORT audiorw-targets FILE_SET HEADERS DESTINATION include/audiorw)
install(EXPORT audiorw-targets FILE audiorw-targets.cmake NAMESPACE audiorw:: DESTINATION lib/cmake/audiorw)
configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/audiorw-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/audiorw-config.cmake"
    INSTALL_DESTINATION lib/cmake/audiorw
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/audiorw-config.cmake" DESTINATION lib/cmake/audiorw)
