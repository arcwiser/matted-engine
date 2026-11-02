cmake_minimum_required(VERSION 3.24)
project(MattedEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/O2 /fp:fast /EHsc /MP /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-O2 -ffast-math)
endif()

# ---------------------------------------------------------------- engine lib
file(GLOB_RECURSE MATTED_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp)
add_library(matted STATIC ${MATTED_ENGINE_SOURCES})
target_include_directories(matted PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------- apps
add_executable(demo apps/demo/main.cpp)
target_link_libraries(demo PRIVATE matted)
if (WIN32)
    target_link_libraries(demo PRIVATE user32 gdi32)
endif()

add_executable(bench apps/bench/main.cpp)
target_link_libraries(bench PRIVATE matted)

# ---------------------------------------------------------------- optional backends (AgentB)
option(MATTED_BACKEND_D3D12 "Build the D3D12 render backend" OFF)
if (MATTED_BACKEND_D3D12)
    file(GLOB MATTED_D3D12_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/render/d3d12/*.cpp)
    if (MATTED_D3D12_SOURCES)
        add_library(matted_d3d12 STATIC ${MATTED_D3D12_SOURCES})
        target_include_directories(matted_d3d12 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(matted_d3d12 PUBLIC matted d3d12 dxgi dxguid)
        target_link_libraries(demo PRIVATE matted_d3d12)
    else()
        message(STATUS "MATTED_BACKEND_D3D12=ON but engine/render/d3d12/ has no sources yet")
    endif()
endif()
