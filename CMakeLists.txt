cmake_minimum_required(VERSION 3.16)
project(showplan
    VERSION 0.1.0
    DESCRIPTION "Clean-room SQL Server showplan XML parser"
    LANGUAGES CXX
)

option(SHOWPLAN_BUILD_TESTS "Build libshowplan tests" ON)
option(SHOWPLAN_INSTALL "Install libshowplan targets" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(showplan STATIC
    src/parser.cpp
    src/anonymize.cpp
    third_party/pugixml/src/pugixml.cpp
)
add_library(showplan::showplan ALIAS showplan)

target_include_directories(showplan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        third_party/pugixml/src
)
target_compile_features(showplan PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(showplan PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

if(SHOWPLAN_BUILD_TESTS)
    enable_testing()
    add_executable(showplan_test_parser tests/test_parser.cpp)
    target_link_libraries(showplan_test_parser PRIVATE showplan)
    add_test(
        NAME showplan_smoke
        COMMAND showplan_test_parser
                ${CMAKE_CURRENT_SOURCE_DIR}/samples/tiny.sqlplan
    )
endif()

if(SHOWPLAN_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS showplan
        EXPORT showplanTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(EXPORT showplanTargets
        FILE showplanTargets.cmake
        NAMESPACE showplan::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/showplan
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/showplanConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/showplanConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/showplanConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/showplan
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/showplanConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/showplanConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/showplan
    )
endif()
