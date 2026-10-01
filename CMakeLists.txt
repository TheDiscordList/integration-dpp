cmake_minimum_required(VERSION 3.16)

project(thediscordlist-dpp
	VERSION 0.1.0
	DESCRIPTION "Official TheDiscordList integration for D++"
	HOMEPAGE_URL "https://thediscordlist.com"
	LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	set(THEDISCORDLIST_DPP_IS_TOP_LEVEL ON)
else()
	set(THEDISCORDLIST_DPP_IS_TOP_LEVEL OFF)
endif()

option(THEDISCORDLIST_DPP_BUILD_TESTS "Build the tests" ${THEDISCORDLIST_DPP_IS_TOP_LEVEL})
option(THEDISCORDLIST_DPP_INSTALL "Generate install rules" ${THEDISCORDLIST_DPP_IS_TOP_LEVEL})

include(GNUInstallDirs)

# D++ built in the same tree (add_subdirectory / FetchContent) names its target `dpp`.
if(TARGET dpp AND NOT TARGET dpp::dpp)
	add_library(dpp::dpp ALIAS dpp)
endif()
if(NOT TARGET dpp::dpp)
	find_package(dpp 10 CONFIG REQUIRED)
endif()
find_package(OpenSSL REQUIRED)

add_library(thediscordlist-dpp STATIC
	src/autoposter.cpp
	src/engine.cpp
	src/webhooks.cpp)
add_library(thediscordlist::dpp ALIAS thediscordlist-dpp)

target_compile_features(thediscordlist-dpp PUBLIC cxx_std_17)
target_include_directories(thediscordlist-dpp PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(thediscordlist-dpp PUBLIC dpp::dpp PRIVATE OpenSSL::Crypto)
set_target_properties(thediscordlist-dpp PROPERTIES
	EXPORT_NAME dpp
	POSITION_INDEPENDENT_CODE ON)

if(THEDISCORDLIST_DPP_BUILD_TESTS)
	enable_testing()
	add_executable(thediscordlist-dpp-tests tests/tests.cpp)
	target_include_directories(thediscordlist-dpp-tests PRIVATE src)
	target_compile_definitions(thediscordlist-dpp-tests PRIVATE
		THEDISCORDLIST_DPP_PROJECT_VERSION="${PROJECT_VERSION}")
	target_link_libraries(thediscordlist-dpp-tests PRIVATE thediscordlist::dpp OpenSSL::Crypto)
	add_test(NAME thediscordlist-dpp-tests COMMAND thediscordlist-dpp-tests)
endif()

if(THEDISCORDLIST_DPP_INSTALL)
	include(CMakePackageConfigHelpers)

	set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/thediscordlist-dpp)

	install(TARGETS thediscordlist-dpp EXPORT thediscordlist-dpp-targets
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
	install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
	install(EXPORT thediscordlist-dpp-targets
		NAMESPACE thediscordlist::
		DESTINATION ${config_dir})

	configure_package_config_file(cmake/thediscordlist-dpp-config.cmake.in
		${CMAKE_CURRENT_BINARY_DIR}/thediscordlist-dpp-config.cmake
		INSTALL_DESTINATION ${config_dir})
	# 0.x: a minor bump may break the API.
	write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/thediscordlist-dpp-config-version.cmake
		COMPATIBILITY SameMinorVersion)
	install(FILES
		${CMAKE_CURRENT_BINARY_DIR}/thediscordlist-dpp-config.cmake
		${CMAKE_CURRENT_BINARY_DIR}/thediscordlist-dpp-config-version.cmake
		DESTINATION ${config_dir})
endif()
