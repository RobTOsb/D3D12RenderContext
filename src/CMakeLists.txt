add_library(D3D12RenderContext STATIC 
        RenderContext.cpp
        D3D12RenderContext.cpp
        Window.cpp
        GPUEncoder.cpp
)

target_include_directories(D3D12RenderContext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(D3D12RenderContext PUBLIC D3D12 DirectXTex fmtlog::fmtlog_static PRIVATE d3dcompiler)

# The BC encode shaders are compiled at runtime straight from the source tree.
target_compile_definitions(D3D12RenderContext PRIVATE
        GPU_ENCODER_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/EncodeShaders/"
)