cmake_minimum_required(VERSION 3.10)
set(SOURCES "")

################ Current Module ################
list(APPEND SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/http.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/mediadb.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/mediascan.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/muslib_get.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/plist.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/server.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/stream.cpp)

################ Submodules ################

################ Exports ################

set(SOURCES ${SOURCES}
	PARENT_SCOPE)
