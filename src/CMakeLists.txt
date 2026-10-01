add_library(D3D12RenderContext STATIC 
        RenderContext.cpp
        D3D12RenderContext.cpp
        Window.cpp        
)

target_include_directories(D3D12RenderContext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(D3D12RenderContext PUBLIC D3D12 DirectXTex fmtlog::fmtlog_static)