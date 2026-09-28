#include "RenderContext.h"
#include "D3D12RenderContext.h"

#include "Window.h"

RenderContext *CreateRenderContext(Window *window, RenderAPI api, uint framesInFlight, bool useSoftwareAdapter, bool useHDR)
{
    switch (api)
    {
    case RenderAPI::D3D12:
        return new D3D12RenderContext(window, framesInFlight, useSoftwareAdapter, useHDR);
    case RenderAPI::Vulkan:
        // Implement VulkanRenderContext creation
        return nullptr;
    case RenderAPI::OpenGL:
        // Implement OpenGLRenderContext creation
        return nullptr;
    case RenderAPI::Metal:
        // Implement MetalRenderContext creation
        return nullptr;
    default:
        throw std::runtime_error("Unsupported RenderAPI");
    }
}