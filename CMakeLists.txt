# Chronos — a modular, real-time audio time-stretching and pitch-shifting engine.
#
# The core library (target `chronos`, aliased `Chronos::chronos`) is dependency-free
# C++17: embed it in a JUCE plugin, another CMake project, or a command-line tool
# with nothing but a compiler.
#
# Tests, the benchmark and the example CLI build only when Chronos is the top-level
# project, so `add_subdirectory(Chronos)` in a host stays lean.

cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CHRONOS_VERSION_RAW)
string(STRIP "${CHRONOS_VERSION_RAW}" CHRONOS_VERSION)

project(Chronos VERSION ${CHRONOS_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHRONOS_BUILD_TESTS    "Build the correctness harness" ${PROJECT_IS_TOP_LEVEL})
option(CHRONOS_BUILD_BENCH    "Build the throughput benchmark" ${PROJECT_IS_TOP_LEVEL})
option(CHRONOS_BUILD_EXAMPLES "Build the file-render CLI"      ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(chronos STATIC
    src/dsp/Fft.cpp
    src/dsp/Window.cpp
    src/dsp/AudioFifo.cpp
    src/dsp/Resampler.cpp
    src/dsp/SincResampler.cpp
    src/analysis/TransientDetector.cpp
    src/analysis/SpectralEnvelope.cpp
    src/engine/PhaseVocoderEngine.cpp
    src/engine/WsolaEngine.cpp
    src/engine/SinusoidalEngine.cpp
    src/TimeStretcher.cpp
)
add_library(Chronos::chronos ALIAS chronos)

target_include_directories(chronos
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(chronos PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(chronos PRIVATE /W4 /O2)
else()
    target_compile_options(chronos PRIVATE -Wall -Wextra -O2)
endif()

set_target_properties(chronos PROPERTIES
    VERSION   ${PROJECT_VERSION}
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# Tests / benchmark / example
# ---------------------------------------------------------------------------
if(CHRONOS_BUILD_TESTS)
    enable_testing()
    add_executable(chronos_tests test/TestMain.cpp)
    target_link_libraries(chronos_tests PRIVATE chronos)
    target_include_directories(chronos_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME chronos_tests COMMAND chronos_tests)
endif()

if(CHRONOS_BUILD_BENCH)
    add_executable(chronos_bench bench/Bench.cpp)
    target_link_libraries(chronos_bench PRIVATE chronos)
    target_include_directories(chronos_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

if(CHRONOS_BUILD_EXAMPLES)
    add_executable(chronos_file examples/chronos_file.cpp)
    target_link_libraries(chronos_file PRIVATE chronos)
    target_include_directories(chronos_file PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif()

# ---------------------------------------------------------------------------
# Install / find_package support
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS chronos EXPORT ChronosTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/chronos DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ChronosTargets
        FILE ChronosTargets.cmake
        NAMESPACE Chronos::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Chronos)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ChronosConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ChronosConfig.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/ChronosTargets.cmake\")\n")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ChronosConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ChronosConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Chronos)
