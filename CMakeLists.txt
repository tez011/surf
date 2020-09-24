cmake_minimum_required(VERSION 3.11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/lib)
cmake_policy(SET CMP0003 NEW)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(surf
	VERSION 0.0.0
	LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
configure_file(${CMAKE_SOURCE_DIR}/include/config.in.h ${CMAKE_BINARY_DIR}/include/config.h)
include_directories(BEFORE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/include)

add_subdirectory(lib)
include_directories(SYSTEM ${IDEPINC})
link_libraries(${IDEPLIB})

if(MSVC)
	# what
else()
	add_compile_options(# "-Wall" "-Wpedantic" "-Wextra"
		"-fexceptions"
		"$<$<CONFIG:Debug>:-O0>"
		"$<$<CONFIG:Debug>:-g3>"
		"$<$<CONFIG:Debug>:-ggdb>"
		"$<$<CONFIG:Debug>:-fsanitize=address>"
		"$<$<CONFIG:Debug>:-fsanitize=undefined>"
		"$<$<CONFIG:Release>:-O3>"
		"$<$<CONFIG:RelWithDebInfo>:-O3>"
		"$<$<CONFIG:RelWithDebInfo>:-g3>"
		"$<$<CONFIG:RelWithDebInfo>:-ggdb>"
		"$<$<CONFIG:MinSizeRel>:-Os>")
	add_link_options(
		"$<$<CONFIG:Debug>:-fsanitize=address>"
		"$<$<CONFIG:Debug>:-fsanitize=undefined>")
endif()

add_subdirectory(src)
add_executable(surf
	${SOURCES})
