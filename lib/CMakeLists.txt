cmake_minimum_required(VERSION 3.11)
include(FetchContent)
find_package(Threads REQUIRED)
set(IDEPSRC "")
set(IDEPINC "")
set(IDEPLIB "")

###################################### inih ######################################
FetchContent_Declare(inih
	GIT_REPOSITORY	https://github.com/benhoyt/inih.git
	GIT_TAG		r51)
FetchContent_GetProperties(inih)
if(NOT inih_POPULATED)
	FetchContent_Populate(inih)
endif()
list(APPEND IDEPINC ${inih_SOURCE_DIR})
list(APPEND IDEPSRC ${inih_SOURCE_DIR}/ini.c)

###################################### json ######################################
FetchContent_Declare(json
	URL		https://github.com/nlohmann/json/releases/download/v3.9.1/include.zip
	URL_HASH	MD5=d2f66c608af689e21d69a33c220e974e)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
	FetchContent_Populate(json)
endif()
list(APPEND IDEPINC ${json_SOURCE_DIR}/single_include)

###################################### picohttpparser ######################################
FetchContent_Declare(h2oparser
	GIT_REPOSITORY	https://github.com/h2o/picohttpparser.git
	GIT_TAG		fe60579f28575def1db78f62f0e4232766c11104)
FetchContent_GetProperties(h2oparser)
if(NOT h2oparser_POPULATED)
	FetchContent_Populate(h2oparser)
endif()
list(APPEND IDEPINC ${h2oparser_SOURCE_DIR})
list(APPEND IDEPSRC ${h2oparser_SOURCE_DIR}/picohttpparser.c)

###################################### platform-folders ######################################
FetchContent_Declare(pfolders
	GIT_REPOSITORY	https://github.com/sago007/PlatformFolders.git
	GIT_TAG		v1.0)
FetchContent_GetProperties(pfolders)
if(NOT pfolders_POPULATED)
	FetchContent_Populate(pfolders)
endif()
list(APPEND IDEPINC ${pfolders_SOURCE_DIR})
list(APPEND IDEPSRC ${pfolders_SOURCE_DIR}/sago/platform_folders.cpp)

###################################### sockpp ######################################
FetchContent_Declare(sockpp
	GIT_REPOSITORY	https://github.com/fpagliughi/sockpp.git
	GIT_TAG		v0.7)
FetchContent_GetProperties(sockpp)
if(NOT sockpp_POPULATED)
	FetchContent_Populate(sockpp)
endif()
set(SOCKPP_BUILD_SHARED OFF CACHE BOOL "")
set(SOCKPP_BUILD_STATIC ON CACHE BOOL "")
add_subdirectory(${sockpp_SOURCE_DIR} ${sockpp_BINARY_DIR} EXCLUDE_FROM_ALL)
list(APPEND IDEPINC ${sockpp_SOURCE_DIR}/include)
list(APPEND IDEPLIB sockpp-static)

###################################### sqlite3 ######################################
FetchContent_Declare(sqlite3
	URL		https://www.sqlite.org/2020/sqlite-amalgamation-3330000.zip
	URL_HASH	MD5=944829c3d88a958be935480b8e56b1fb)
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
	FetchContent_Populate(sqlite3)
endif()
list(APPEND IDEPINC ${sqlite3_SOURCE_DIR})
list(APPEND IDEPSRC ${sqlite3_SOURCE_DIR}/sqlite3.c)
add_compile_definitions(SQLITE_OMIT_LOAD_EXTENSION)

###################################### ffmpeg ######################################
find_file(AVCODEC_INC libavcodec/avcodec.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_file(AVFILTER_INC libavfilter/avfilter.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_file(AVFORMAT_INC libavformat/avformat.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_file(AVUTIL_INC libavutil/avutil.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_file(SWRESAMPLE_INC libswresample/swresample.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_library(AVCODEC_LIB avcodec HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)
find_library(AVFILTER_LIB avfilter HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)
find_library(AVFORMAT_LIB avformat HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)
find_library(AVUTIL_LIB avutil HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)
find_library(SWRESAMPLE_LIB swresample HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)

get_filename_component(AVCODEC_INC ${AVCODEC_INC} DIRECTORY)
get_filename_component(AVCODEC_INC ${AVCODEC_INC} DIRECTORY)
get_filename_component(AVFILTER_INC ${AVFILTER_INC} DIRECTORY)
get_filename_component(AVFILTER_INC ${AVFILTER_INC} DIRECTORY)
get_filename_component(AVFORMAT_INC ${AVFORMAT_INC} DIRECTORY)
get_filename_component(AVFORMAT_INC ${AVFORMAT_INC} DIRECTORY)
get_filename_component(AVUTIL_INC ${AVUTIL_INC} DIRECTORY)
get_filename_component(AVUTIL_INC ${AVUTIL_INC} DIRECTORY)
get_filename_component(SWRESAMPLE_INC ${SWRESAMPLE_INC} DIRECTORY)
get_filename_component(SWRESAMPLE_INC ${SWRESAMPLE_INC} DIRECTORY)

list(APPEND IDEPINC ${AVCODEC_INC} ${AVFILTER_INC} ${AVFORMAT_INC} ${AVUTIL_INC} ${SWRESAMPLE_INC})
list(APPEND IDEPLIB ${AVCODEC_LIB} ${AVFILTER_LIB} ${AVFORMAT_LIB} ${AVUTIL_LIB} ${SWRESAMPLE_LIB})


###################################### highwayhash ######################################
find_file(HHASH_INC highwayhash/highwayhash.h HINTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_library(HHASH_LIB highwayhash HINTS ${CMAKE_CURRENT_SOURCE_DIR}/lib)
get_filename_component(HHASH_INC ${HHASH_INC} DIRECTORY)
get_filename_component(HHASH_INC ${HHASH_INC} DIRECTORY)

list(APPEND IDEPINC ${HHASH_INC})
list(APPEND IDEPLIB ${HHASH_LIB})

############################################################################
add_library(ideps OBJECT EXCLUDE_FROM_ALL ${IDEPSRC})
target_include_directories(ideps SYSTEM PRIVATE ${IDEPINC})
set(IDEPINC ${IDEPINC} PARENT_SCOPE)
set(IDEPLIB ${IDEPLIB} ideps pthread PARENT_SCOPE)
