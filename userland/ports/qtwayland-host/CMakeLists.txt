cmake_minimum_required(VERSION 3.16)
project(QtWaylandHostTools VERSION 6.8.3 LANGUAGES CXX)
# Only the generator is native. Never execute a target-architecture tool during
# a cross build, and do not require an unrelated native Qt GUI/compositor stack.
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)
if(NOT EXISTS "${QTWAYLAND_SOURCE_DIR}/src/qtwaylandscanner/qtwaylandscanner.cpp")
    message(FATAL_ERROR "QTWAYLAND_SOURCE_DIR must name the verified QtWayland source")
endif()
add_executable(qtwaylandscanner "${QTWAYLAND_SOURCE_DIR}/src/qtwaylandscanner/qtwaylandscanner.cpp")
target_compile_features(qtwaylandscanner PRIVATE cxx_std_17)
target_link_libraries(qtwaylandscanner PRIVATE Qt6::Core)
set_target_properties(qtwaylandscanner PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
install(TARGETS qtwaylandscanner EXPORT Qt6WaylandScannerToolsTargets RUNTIME DESTINATION libexec)
install(EXPORT Qt6WaylandScannerToolsTargets NAMESPACE Qt6:: DESTINATION lib/cmake/Qt6WaylandScannerTools)
include(CMakePackageConfigHelpers)
configure_package_config_file(Qt6WaylandScannerToolsConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/Qt6WaylandScannerToolsConfig.cmake"
    INSTALL_DESTINATION lib/cmake/Qt6WaylandScannerTools)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/Qt6WaylandScannerToolsConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/Qt6WaylandScannerToolsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/Qt6WaylandScannerToolsConfigVersion.cmake"
    DESTINATION lib/cmake/Qt6WaylandScannerTools)
