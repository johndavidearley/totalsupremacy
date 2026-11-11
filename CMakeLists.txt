cmake_minimum_required(VERSION 3.20)
project(TotalSupremacy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(APPLE)
  list(PREPEND CMAKE_PREFIX_PATH /opt/local)
  message(STATUS "Searching for Vulkan and GLFW in the MacPorts prefix /opt/local")
endif()

add_library(totalsupremacy_core
  src/SceneGeometry.cpp
  src/Simulation.cpp
)
target_include_directories(totalsupremacy_core PUBLIC include)
if(MSVC)
  target_compile_options(totalsupremacy_core PRIVATE /W4)
else()
  target_compile_options(totalsupremacy_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
if(APPLE)
  find_program(TOTALSUPREMACY_GLSLC glslc HINTS /opt/local/bin REQUIRED)
else()
  find_program(TOTALSUPREMACY_GLSLC glslc REQUIRED)
endif()

set(TOTALSUPREMACY_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${TOTALSUPREMACY_SHADER_DIR}")
foreach(shader scene.vert scene.frag pathfinding.comp)
  add_custom_command(
    OUTPUT "${TOTALSUPREMACY_SHADER_DIR}/${shader}.spv"
    COMMAND "${TOTALSUPREMACY_GLSLC}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
            -o "${TOTALSUPREMACY_SHADER_DIR}/${shader}.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
    VERBATIM
  )
  list(APPEND TOTALSUPREMACY_SHADER_BINARIES
       "${TOTALSUPREMACY_SHADER_DIR}/${shader}.spv")
endforeach()
add_custom_target(totalsupremacy_shaders DEPENDS ${TOTALSUPREMACY_SHADER_BINARIES})

add_executable(TotalSupremacy src/main.cpp)
if(MSVC)
  target_compile_options(TotalSupremacy PRIVATE /W4)
else()
  target_compile_options(TotalSupremacy PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_sources(TotalSupremacy PRIVATE src/ControlBridge.cpp src/Renderer3D.cpp)
add_dependencies(TotalSupremacy totalsupremacy_shaders)
target_compile_definitions(TotalSupremacy PRIVATE
  TOTALSUPREMACY_SHADER_DIR="${TOTALSUPREMACY_SHADER_DIR}"
)
if(TARGET glfw)
  set(TOTALSUPREMACY_GLFW_TARGET glfw)
elseif(TARGET glfw3::glfw)
  set(TOTALSUPREMACY_GLFW_TARGET glfw3::glfw)
else()
  message(FATAL_ERROR "The glfw3 package did not define a supported GLFW target.")
endif()
target_link_libraries(TotalSupremacy PRIVATE
  totalsupremacy_core
  Vulkan::Vulkan
  ${TOTALSUPREMACY_GLFW_TARGET}
)

add_executable(totalsupremacy_benchmark benchmarks/SimulationBenchmark.cpp)
target_link_libraries(totalsupremacy_benchmark PRIVATE totalsupremacy_core)
target_compile_definitions(totalsupremacy_benchmark PRIVATE
  TOTALSUPREMACY_BUILD_TYPE="$<CONFIG>"
)
if(MSVC)
  target_compile_options(totalsupremacy_benchmark PRIVATE /W4)
else()
  target_compile_options(totalsupremacy_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(totalsupremacy_gpu_benchmark benchmarks/GpuPathfindingBenchmark.cpp)
add_dependencies(totalsupremacy_gpu_benchmark totalsupremacy_shaders)
target_link_libraries(totalsupremacy_gpu_benchmark PRIVATE
  totalsupremacy_core Vulkan::Vulkan)
target_compile_definitions(totalsupremacy_gpu_benchmark PRIVATE
  TOTALSUPREMACY_SHADER_DIR="${TOTALSUPREMACY_SHADER_DIR}"
)
if(MSVC)
  target_compile_options(totalsupremacy_gpu_benchmark PRIVATE /W4)
else()
  target_compile_options(totalsupremacy_gpu_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(totalsupremacy_scene_benchmark benchmarks/SceneBenchmark.cpp)
target_link_libraries(totalsupremacy_scene_benchmark PRIVATE totalsupremacy_core)
target_compile_definitions(totalsupremacy_scene_benchmark PRIVATE
  TOTALSUPREMACY_BUILD_TYPE="$<CONFIG>"
)
if(MSVC)
  target_compile_options(totalsupremacy_scene_benchmark PRIVATE /W4)
else()
  target_compile_options(totalsupremacy_scene_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(totalsupremacy_renderer_benchmark
  benchmarks/RendererBenchmark.cpp
  src/Renderer3D.cpp
)
add_dependencies(totalsupremacy_renderer_benchmark totalsupremacy_shaders)
target_compile_definitions(totalsupremacy_renderer_benchmark PRIVATE
  TOTALSUPREMACY_SHADER_DIR="${TOTALSUPREMACY_SHADER_DIR}"
  TOTALSUPREMACY_BUILD_TYPE="$<CONFIG>"
)
if(MSVC)
  target_compile_options(totalsupremacy_renderer_benchmark PRIVATE /W4)
else()
  target_compile_options(totalsupremacy_renderer_benchmark PRIVATE
    -Wall -Wextra -Wpedantic
  )
endif()
target_link_libraries(totalsupremacy_renderer_benchmark PRIVATE
  totalsupremacy_core
  Vulkan::Vulkan
  ${TOTALSUPREMACY_GLFW_TARGET}
)

if(APPLE)
  find_library(TOTALSUPREMACY_MOLTENVK_LIBRARY NAMES MoltenVK
    PATHS /opt/local/lib NO_DEFAULT_PATH REQUIRED)
  foreach(target TotalSupremacy totalsupremacy_renderer_benchmark totalsupremacy_gpu_benchmark)
    target_link_libraries(${target} PRIVATE "${TOTALSUPREMACY_MOLTENVK_LIBRARY}")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH /opt/local/lib)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(totalsupremacy_tests tests/SimulationTests.cpp)
  target_link_libraries(totalsupremacy_tests PRIVATE totalsupremacy_core)
  add_test(NAME totalsupremacy_tests COMMAND totalsupremacy_tests)

  find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME mcp_control_tests
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mcp/tests/test_control.py
              $<TARGET_FILE:TotalSupremacy>
    )
    set_tests_properties(mcp_control_tests PROPERTIES TIMEOUT 20)
    execute_process(
      COMMAND ${Python3_EXECUTABLE} -c "import mcp"
      RESULT_VARIABLE TOTALSUPREMACY_MCP_SDK_AVAILABLE
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if(TOTALSUPREMACY_MCP_SDK_AVAILABLE EQUAL 0)
      add_test(NAME mcp_server_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mcp/tests/test_mcp_server.py
                $<TARGET_FILE:TotalSupremacy>
      )
      set_tests_properties(mcp_server_tests PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools/mcp/src"
      )
    endif()
  endif()
endif()
