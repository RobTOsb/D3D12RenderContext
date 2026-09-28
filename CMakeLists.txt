
cmake_minimum_required(VERSION 3.28)
                
project("D3D12RenderContext")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(cmake/CPM.cmake)

add_subdirectory("external/D3D12Wrapper")
add_subdirectory("src")
