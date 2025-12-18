cmake_minimum_required(VERSION 3.30)
project(audiorw-test)
add_executable(audiorw-test)
target_sources(audiorw-test PRIVATE
	src/doctest.h
	src/main.cpp
)
target_link_libraries(audiorw-test audiorw::audiorw)
target_compile_definitions(audiorw-test PRIVATE
	ASSETS_DIR="${CMAKE_CURRENT_LIST_DIR}/assets"
)
add_test(NAME audiorw-test COMMAND audiorw-test)
