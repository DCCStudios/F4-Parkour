cmake_minimum_required(VERSION 3.21)

macro(set_from_environment VARIABLE)
	if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif ()
endmacro()

set_from_environment(VCPKG_ROOT)
set_from_environment(Fallout4Path)

if (DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else()
	message(WARNING "VCPKG_ROOT is not set. Continuing without vcpkg.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(
	F4Parkour
	VERSION 1.0.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/cmake")

if (NOT TARGET CommonLibF4)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/CommonLibF4" CommonLibF4)
endif()

find_package(spdlog REQUIRED CONFIG)

set(SOURCES
	src/PCH.cpp
	src/main.cpp
	src/Settings.cpp
	src/ParkourUtility.cpp
	src/Parkouring.cpp
	src/AnimEventSink.cpp
	src/HUD.cpp
)

set(HEADERS
	src/PCH.h
	src/ParkourTypes.h
	src/HardcodedVariables.h
	src/Settings.h
	src/ParkourUtility.h
	src/Parkouring.h
	src/AnimEventSink.h
	src/HUD.h
	src/SimpleIni.h
	src/F4SEMenuFramework.h
)

add_library(${PROJECT_NAME} SHARED
	${SOURCES}
	${HEADERS}
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	_UNICODE
	UNICODE
	NOMINMAX
	_CRT_SECURE_NO_WARNINGS
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
	CommonLibF4::CommonLibF4
	spdlog::spdlog
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
	src/PCH.h
)

if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE
		/sdl
		/utf-8
		/Zi
		/permissive-
		/Zc:preprocessor
		/EHsc
		/W4
		/WX-
		/wd4099
		/wd4100
		/wd4189
		/wd4244
		/wd4302
		/wd4311
		/wd5054
	)

	target_link_options(${PROJECT_NAME} PRIVATE
		"$<$<CONFIG:DEBUG>:/INCREMENTAL>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif()

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/F4Parkour.ini
	${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins/F4Parkour.ini
	COPYONLY
)

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)
if (COPY_BUILD AND DEFINED Fallout4Path)
	add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/F4Parkour.ini" "${Fallout4Path}/Data/F4SE/Plugins/"
	)
endif()
