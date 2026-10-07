#include "D3D12RenderContext.h"


#include "D3D12ImguiRenderer.h"
#include "Window.h"

#include "fmtlog.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

#undef min
#undef max

// namespace
// {
// 	class D3D12TextureCompressor final : public ITextureCompressor
// 	{
// 	public:
// 		D3D12TextureCompressor(D3D12Device *device, D3D12CommandQueue *computeQueue, D3D12DescriptorHeap *heap) :
// 			encoder_(device, computeQueue, heap)
// 		{
// 		}

// 		bool Initialise() override
// 		{
// 			return encoder_.Initialise();
// 		}

// 		bool IsReady() const override
// 		{
// 			return encoder_.IsReady();
// 		}

// 		HRESULT Compress(const DirectX::ScratchImage &srcChain,
// 						 DXGI_FORMAT bcFormat,
// 						 DirectX::ScratchImage &result) override
// 		{
// 			return encoder_.Compress(srcChain, bcFormat, result);
// 		}

// 	private:
// 		GPUEncoder encoder_;
// 	};
// } // namespace

D3D12RenderContext::D3D12RenderContext(Window *window, uint framesInFlight, bool useSoftwareAdapter, bool useHDR)
{
	device_ = std::make_unique<D3D12Device>(useSoftwareAdapter);
	graphicsQueue_ = std::make_unique<D3D12CommandQueue>(device_->GetNativeDevice(), D3D12QueueType::Graphics);
	copyQueue_ = std::make_unique<D3D12CommandQueue>(device_->GetNativeDevice(), D3D12QueueType::Copy);
	computeQueue_ = std::make_unique<D3D12CommandQueue>(device_->GetNativeDevice(), D3D12QueueType::Compute);

	swapchain_ = device_->CreateSwapchain(graphicsQueue_.get(),
										  window->GetNativeWindow(),
										  window->GetWidth(),
										  window->GetHeight(),
										  framesInFlight,
										  useHDR);

	resourceDescriptorHeap_ =
			std::make_unique<D3D12DescriptorHeap>(device_->GetNativeDevice(),
												  1'000'000, // max size of the resource descriptor heap
												  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
												  framesInFlight);

	// Same size/index space as resourceDescriptorHeap_, just non-shader-visible: lets
	// ClearUAVBufferUint create a CPU-only view at the same index as an existing resource's
	// shader-visible one, which ClearUnorderedAccessViewUint requires.
	cpuOnlyDescriptorHeap_ = std::make_unique<D3D12DescriptorHeap>(device_->GetNativeDevice(),
																   1'000'000,
																   D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

	shaderCompiler_ = std::make_unique<SlangShaderCompiler>();

	framesInFlight_ = framesInFlight;

	windowWidth_ = window->GetWidth();
	windowHeight_ = window->GetHeight();
	useHDR_ = useHDR;

	RegisterSwapchainBackBuffers();
}

D3D12RenderContext::~D3D12RenderContext()
{
	if (!perFrameResources_.empty())
	{
		WaitForGPUIdle();

		D3D12Fence shutdownFence(device_->GetNativeDevice(), 0);
		shutdownFence.Signal(graphicsQueue_.get(), 1);
		shutdownFence.Wait(1);
	}
}

void D3D12RenderContext::CreatePerFrameResources()
{
	perFrameResources_.resize(framesInFlight_);

	for (uint i = 0; i < framesInFlight_; ++i)
	{
		perFrameResources_[i].graphicsCommandAllocator =
				device_->CreateCommandAllocator(D3D12CommandListType::Graphics);
		perFrameResources_[i].graphicsCommandList = perFrameResources_[i].graphicsCommandAllocator->CreateCommandList();
		perFrameResources_[i].fence = device_->CreateFence(0);
		perFrameResources_[i].fenceValue = 0;
	}

	gpuProfiler_ = std::make_unique<D3D12GPUProfiler>(device_.get(), graphicsQueue_.get(), framesInFlight_);
}

void D3D12RenderContext::BeginFrame()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	if (frameResources.fence->GetCompletedValue() < frameResources.fenceValue)
	{
		frameResources.fence->Wait(frameResources.fenceValue);
	}

	frameResources.graphicsCommandAllocator->Reset();

	// The fence wait above means this slot's shader visible heaps are no longer in use by the GPU.
	resourceDescriptorHeap_->BeginFrame(currentFrameIndex_);

	// This slot's fence has passed, so the timestamps it resolved last time round are readable.
	gpuProfiler_->BeginFrame(currentFrameIndex_);
}

void D3D12RenderContext::EndFrame()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.fenceValue++;

	const SubmitSyncInfo syncInfo = { .fence = frameResources.fence.get(),
									  .signalValue = frameResources.fenceValue,
									  .waitValues = nullptr };

	// Descriptors are read when the command list executes, so anything written while recording
	// this frame still makes it in.
	resourceDescriptorHeap_->Flush();

	D3D12CommandList *commandLists[] = { frameResources.graphicsCommandList.get() };
	graphicsQueue_->ExecuteCommandLists(commandLists, 1, syncInfo);

	currentFrame_++;
	currentFrameIndex_ = currentFrame_ % framesInFlight_;
}

void D3D12RenderContext::BeginGraphicsCommandList()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->BeginCommandList();
}

void D3D12RenderContext::EndGraphicsCommandList()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	gpuProfiler_->EndFrame(frameResources.graphicsCommandList.get());
	frameResources.graphicsCommandList->EndCommandList();
}

void D3D12RenderContext::WaitForGPUIdle()
{
	for (uint i = 0; i < framesInFlight_; ++i)
	{
		auto &frameResources = perFrameResources_[i];
		if (frameResources.fence->GetCompletedValue() < frameResources.fenceValue)
		{
			frameResources.fence->Wait(frameResources.fenceValue);
		}
	}
}

void D3D12RenderContext::Present()
{
	swapchain_->Present(0U, DXGI_PRESENT_ALLOW_TEARING);
}

void D3D12RenderContext::InitialiseImgui(void *hwnd)
{
	imguiRenderer_ = std::make_unique<D3D12ImguiRenderer>(device_.get(),
														  swapchain_->GetBackBufferFormat(),
														  graphicsQueue_.get(),
														  resourceDescriptorHeap_.get(),
														  hwnd,
														  framesInFlight_);
}

void D3D12RenderContext::ImguiNewFrame()
{
	imguiRenderer_->NewFrame();
}

void D3D12RenderContext::ImguiRender()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	imguiRenderer_->RenderUI(frameResources.graphicsCommandList.get());
}

static const wchar_t *TargetProfileForStage(PipelineShaderStage stage)
{
	switch (stage)
	{
		case PipelineShaderStage::Vertex:
			return L"vs_6_6";
		case PipelineShaderStage::Pixel:
			return L"ps_6_6";
		case PipelineShaderStage::Amplification:
			return L"as_6_6";
		case PipelineShaderStage::Mesh:
			return L"ms_6_6";
		default:
			return L"";
	}
}

// Binding a root signature leaves every root argument undefined, and each table binding now owns a
// root parameter, so point them all at the heap start first. Kernels routinely declare a texture
// they never bind on a given dispatch; this keeps that parameter defined rather than undefined.
template<typename SetTableFn>
static void DefaultInitialiseDescriptorTables(const ReflectedRootSignature &rootSignature,
											  GPUDescriptorHandle heapStart,
											  SetTableFn setTable)
{
	for (const ReflectedRootBinding &binding: rootSignature.bindings)
	{
		if (binding.isDescriptorTable)
		{
			setTable(binding.rootParameterIndex, heapStart);
		}
	}
}

void D3D12RenderContext::BeginRootParameterTracking(RootParameterTracker &tracker,
													const ReflectedRootSignature &rootSignature)
{
#ifndef NDEBUG
	tracker.rootSignature = &rootSignature;
	tracker.setMask = 0;

	// Descriptor tables are excluded: DefaultInitialiseDescriptorTables has just defined them all.
	tracker.requiredMask = 0;
	for (const ReflectedRootBinding &binding: rootSignature.bindings)
	{
		if (!binding.isDescriptorTable && binding.rootParameterIndex < 64)
		{
			tracker.requiredMask |= (1ull << binding.rootParameterIndex);
		}
	}
	if (rootSignature.rootConstantParameterIndex != kNoRootConstantParameter &&
		rootSignature.rootConstantParameterIndex < 64)
	{
		tracker.requiredMask |= (1ull << rootSignature.rootConstantParameterIndex);
	}
#else
	(void) tracker;
	(void) rootSignature;
#endif
}

void D3D12RenderContext::MarkRootParameterSet(const ReflectedRootSignature &rootSignature, uint32_t rootParameterIndex)
{
#ifndef NDEBUG
	if (rootParameterIndex >= 64)
	{
		return;
	}
	for (RootParameterTracker *tracker:
		 { &graphicsRootParameters_, &computeRootParameters_, &raytracingRootParameters_ })
	{
		if (tracker->rootSignature == &rootSignature)
		{
			tracker->setMask |= (1ull << rootParameterIndex);
			return;
		}
	}
#else
	(void) rootSignature;
	(void) rootParameterIndex;
#endif
}

void D3D12RenderContext::ValidateRootParameters(const char *callSite)
{
#ifndef NDEBUG
	const RootParameterTracker *tracker = nullptr;
	switch (lastBoundPipelineKind_)
	{
		case PipelineKind::Graphics:
			tracker = &graphicsRootParameters_;
			break;
		case PipelineKind::Compute:
			tracker = &computeRootParameters_;
			break;
		case PipelineKind::Raytracing:
			tracker = &raytracingRootParameters_;
			break;
		default:
			return;
	}

	const uint64_t missing = tracker->requiredMask & ~tracker->setMask;
	if (missing == 0)
	{
		return;
	}

	static const char *const kRangeTypeNames[] = { "t", "u", "b", "s" };
	for (const ReflectedRootBinding &binding: tracker->rootSignature->bindings)
	{
		if (binding.isDescriptorTable || binding.rootParameterIndex >= 64 ||
			(missing & (1ull << binding.rootParameterIndex)) == 0)
		{
			continue;
		}
		if (!reportedRootParameters_.emplace(tracker->rootSignature, binding.rootParameterIndex).second)
		{
			continue;
		}
		loge("{}: root descriptor '{}' ({}{}, root parameter {}) was never set - the shader will read "
			 "an undefined address",
			 callSite,
			 binding.name,
			 kRangeTypeNames[static_cast<size_t>(binding.type) % 4],
			 binding.baseShaderRegister,
			 binding.rootParameterIndex);
	}

	const uint32_t rootConstantIndex = tracker->rootSignature->rootConstantParameterIndex;
	if (rootConstantIndex != kNoRootConstantParameter && rootConstantIndex < 64 &&
		(missing & (1ull << rootConstantIndex)) != 0 &&
		reportedRootParameters_.emplace(tracker->rootSignature, rootConstantIndex).second)
	{
		loge("{}: root constants (root parameter {}) were never set", callSite, rootConstantIndex);
	}
#else
	(void) callSite;
#endif
}

// Finds a binding by predicate across whichever reflected root signature is currently bound
// (graphics/compute/raytracing all share this shape), and routes it to a descriptor-table or
// root-descriptor bind call. Used by every SetXXX/SetXXXByName overload below.
template<typename Predicate, typename SetTableFn, typename SetRootFn>
static void BindResourceByPredicate(const ReflectedRootSignature &rootSignature,
									D3D12RenderContext &context,
									ResourceHandle resource,
									Predicate predicate,
									SetTableFn setTable,
									SetRootFn setRoot,
									const char *notFoundMessage,
									bool required = true)
{
	for (const ReflectedRootBinding &binding: rootSignature.bindings)
	{
		if (!predicate(binding))
		{
			continue;
		}

		if (binding.isDescriptorTable)
		{
			const ResourceViewInfo info = context.GetResourceViewInfo(resource);
			D3D12_GPU_DESCRIPTOR_HANDLE nativeHandle = {};
			nativeHandle.ptr = static_cast<UINT64>(info.gpuDescriptorHandle);
			setTable(binding.rootParameterIndex, GPUDescriptorHandle(nativeHandle));
		}
		else
		{
			const Resource genericResource = context.GetResource(resource);
			auto *resourcePtr = static_cast<D3D12Resource *>(genericResource.ptr);
			setRoot(binding.rootParameterIndex, resourcePtr->GetResource()->GetGPUVirtualAddress());
			context.MarkRootParameterSet(rootSignature, binding.rootParameterIndex);
		}
		return;
	}

	// Name-based binds are tolerant of "not reached by this entry point" - many GI1 kernels share
	// one header declaring dozens of buffers/textures but only reflect the subset they actually use.
	if (required)
	{
		throw std::runtime_error(notFoundMessage);
	}
}

void D3D12RenderContext::SetGraphicsPipeline(PipelineHandle pipelineHandle)
{
	auto it = graphicsPipelines_.find(pipelineHandle);
	if (it != graphicsPipelines_.end())
	{
		auto &frameResources = perFrameResources_[currentFrameIndex_];
		frameResources.graphicsCommandList->SetGraphicsPipeline(it->second.get());
		currentGraphicsPipeline_ = it->second.get();

		lastBoundPipelineKind_ = PipelineKind::Graphics;
		BeginRootParameterTracking(graphicsRootParameters_, currentGraphicsPipeline_->GetReflectedRootSignature());

		DefaultInitialiseDescriptorTables(
				currentGraphicsPipeline_->GetReflectedRootSignature(),
				resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
				[&](uint32_t param, GPUDescriptorHandle handle)
				{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); });
	}
	else
	{
		throw std::runtime_error("Invalid pipeline handle.");
	}
}

static bool ByRegister(const ReflectedRootBinding &binding, D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t shaderRegister)
{
	return binding.type == type && binding.baseShaderRegister == shaderRegister;
}

static bool ByName(const ReflectedRootBinding &binding, D3D12_DESCRIPTOR_RANGE_TYPE type, const std::string &name)
{
	return binding.type == type && binding.name == name;
}

void D3D12RenderContext::SetSRV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetSRV: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootSRV(param, va); },
			"SetSRV: no SRV binding found at the given shader register.");
}

void D3D12RenderContext::SetUAV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetUAV: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootUAV(param, va); },
			"SetUAV: no UAV binding found at the given shader register.");
}

void D3D12RenderContext::SetConstantBuffer(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetConstantBuffer: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootCBV(param, va); },
			"SetConstantBuffer: no CBV binding found at the given shader register.");
}

void D3D12RenderContext::SetSRVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetSRVByName: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootSRV(param, va); },
			"SetSRVByName: no SRV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetUAVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetUAVByName: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootUAV(param, va); },
			"SetUAVByName: no UAV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetConstantBufferByName(const std::string &name, ResourceHandle resource)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetConstantBufferByName: no graphics pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentGraphicsPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetGraphicsDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetGraphicsRootCBV(param, va); },
			"SetConstantBufferByName: no CBV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetComputeSRV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeSRV: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootSRV(param, va); },
			"SetComputeSRV: no SRV binding found at the given shader register.");
}

void D3D12RenderContext::SetComputeUAV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeUAV: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootUAV(param, va); },
			"SetComputeUAV: no UAV binding found at the given shader register.");
}

void D3D12RenderContext::SetComputeConstantBuffer(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeConstantBuffer: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootCBV(param, va); },
			"SetComputeConstantBuffer: no CBV binding found at the given shader register.");
}

void D3D12RenderContext::SetComputeSRVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeSRVByName: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootSRV(param, va); },
			"SetComputeSRVByName: no SRV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetComputeUAVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeUAVByName: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootUAV(param, va); },
			"SetComputeUAVByName: no UAV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetComputeUAVTableByName(const std::string &name, ResourceHandle firstInArray)
{
	// Binding an unbounded array table works identically to a single-element one: the root
	// parameter is the same descriptor-table type either way, and D3D12 reads however many
	// descriptors the shader actually indexes starting from the bound GPU handle.
	SetComputeUAVByName(name, firstInArray);
}

void D3D12RenderContext::SetComputeSRVTableByName(const std::string &name, ResourceHandle firstInArray)
{
	SetComputeSRVByName(name, firstInArray);
}

void D3D12RenderContext::SetUAVTableByName(const std::string &name, ResourceHandle firstInArray)
{
	SetUAVByName(name, firstInArray);
}

void D3D12RenderContext::SetComputeConstantBufferByName(const std::string &name, ResourceHandle resource)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeConstantBufferByName: no compute pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentComputePipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootCBV(param, va); },
			"SetComputeConstantBufferByName: no CBV binding found with the given name.",
			false);
}

ResourceHandle D3D12RenderContext::GetOrCreateScalarConstantBuffer(const void *pipeline,
																   const std::string &cbufferName,
																   uint32_t sizeInBytes)
{
	const ScalarCBKey key{ pipeline, cbufferName };
	auto it = scalarConstantBuffers_.find(key);
	if (it != scalarConstantBuffers_.end())
	{
		return it->second;
	}

	const ResourceHandle handle = CreateConstantBuffer(sizeInBytes, "ScalarCB_" + cbufferName);
	scalarConstantBuffers_.emplace(key, handle);
	return handle;
}

void D3D12RenderContext::SetComputeScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetComputeScalarByName: no compute pipeline is currently bound.");
	}

	const auto &scalars = currentComputePipeline_->GetReflectedRootSignature().scalars;
	auto it =
			std::find_if(scalars.begin(), scalars.end(), [&name](const ReflectedScalar &s) { return s.name == name; });
	if (it == scalars.end())
	{
		throw std::runtime_error("SetComputeScalarByName: no reflected variable found with the given name.");
	}
	if (sizeInBytes > it->byteSize)
	{
		throw std::runtime_error("SetComputeScalarByName: data is larger than the reflected variable.");
	}

	const ResourceHandle cbHandle =
			GetOrCreateScalarConstantBuffer(currentComputePipeline_, it->cbufferName, it->cbufferByteSize);

	auto resIt = resources_.find(cbHandle);
	if (resIt == resources_.end() || !resIt->second.mappedData)
	{
		throw std::runtime_error("SetComputeScalarByName: scalar constant buffer is not mapped.");
	}
	std::memcpy(static_cast<uint8_t *>(resIt->second.mappedData) + it->byteOffset, data, sizeInBytes);

	SetComputeConstantBufferByName(it->cbufferName, cbHandle);
}

// Raytracing dispatches share the compute root-binding state (SetRaytracingPipeline calls
// SetComputeRootSignature under the hood), so these route through the same command-list calls as
// the compute setters above, just validated against currentRaytracingPipeline_ instead.

void D3D12RenderContext::SetRaytracingSRV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingSRV: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootSRV(param, va); },
			"SetRaytracingSRV: no SRV binding found at the given shader register.");
}

void D3D12RenderContext::SetRaytracingUAV(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingUAV: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootUAV(param, va); },
			"SetRaytracingUAV: no UAV binding found at the given shader register.");
}

void D3D12RenderContext::SetRaytracingConstantBuffer(uint32_t shaderRegister, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingConstantBuffer: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[shaderRegister](const ReflectedRootBinding &b)
			{ return ByRegister(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, shaderRegister); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootCBV(param, va); },
			"SetRaytracingConstantBuffer: no CBV binding found at the given shader register.");
}

void D3D12RenderContext::SetRaytracingSRVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingSRVByName: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootSRV(param, va); },
			"SetRaytracingSRVByName: no SRV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetRaytracingUAVByName(const std::string &name, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingUAVByName: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootUAV(param, va); },
			"SetRaytracingUAVByName: no UAV binding found with the given name.",
			false);
}

void D3D12RenderContext::SetRaytracingConstantBufferByName(const std::string &name, ResourceHandle resource)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingConstantBufferByName: no raytracing pipeline is currently bound.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	BindResourceByPredicate(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			*this,
			resource,
			[&name](const ReflectedRootBinding &b) { return ByName(b, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, name); },
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); },
			[&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS va)
			{ frameResources.graphicsCommandList->SetComputeRootCBV(param, va); },
			"SetRaytracingConstantBufferByName: no CBV binding found with the given name.",
			false);
}

// Raytracing dispatches share the compute root-binding state (see SetRaytracingSRV et al above),
// so this is SetComputeScalarByName's twin, validated against currentRaytracingPipeline_ instead.
void D3D12RenderContext::SetRaytracingScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracingScalarByName: no raytracing pipeline is currently bound.");
	}

	const auto &scalars = currentRaytracingPipeline_->GetReflectedRootSignature().scalars;
	auto it =
			std::find_if(scalars.begin(), scalars.end(), [&name](const ReflectedScalar &s) { return s.name == name; });
	if (it == scalars.end())
	{
		throw std::runtime_error("SetRaytracingScalarByName: no reflected variable found with the given name.");
	}
	if (sizeInBytes > it->byteSize)
	{
		throw std::runtime_error("SetRaytracingScalarByName: data is larger than the reflected variable.");
	}

	const ResourceHandle cbHandle =
			GetOrCreateScalarConstantBuffer(currentRaytracingPipeline_, it->cbufferName, it->cbufferByteSize);

	auto resIt = resources_.find(cbHandle);
	if (resIt == resources_.end() || !resIt->second.mappedData)
	{
		throw std::runtime_error("SetRaytracingScalarByName: scalar constant buffer is not mapped.");
	}
	std::memcpy(static_cast<uint8_t *>(resIt->second.mappedData) + it->byteOffset, data, sizeInBytes);

	SetRaytracingConstantBufferByName(it->cbufferName, cbHandle);
}

void D3D12RenderContext::SetDescriptorHeaps()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetDescriptorHeaps(resourceDescriptorHeap_.get());
}

void D3D12RenderContext::BeginEvent(const std::string &label)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->BeginEvent(label.c_str());
}

void D3D12RenderContext::EndEvent()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->EndEvent();
}

void D3D12RenderContext::BeginProfileScope(const std::string &name)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	gpuProfiler_->BeginScope(frameResources.graphicsCommandList.get(), name);
}

void D3D12RenderContext::EndProfileScope()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	gpuProfiler_->EndScope(frameResources.graphicsCommandList.get());
}

const GPUProfileScopeStats *D3D12RenderContext::GetProfileScopeStats(const std::string &name) const
{
	return gpuProfiler_ ? gpuProfiler_->FindStats(name) : nullptr;
}

std::vector<GPUProfileScopeStats> D3D12RenderContext::GetAllProfileScopeStats() const
{
	return gpuProfiler_ ? gpuProfiler_->GetAllStats() : std::vector<GPUProfileScopeStats>{};
}

void D3D12RenderContext::ResetProfiler()
{
	if (gpuProfiler_)
	{
		gpuProfiler_->Reset();
	}
}

void D3D12RenderContext::SetRenderTargets(const Resource *renderTargets,
										  uint numRenderTargets,
										  Resource depthStencil,
										  bool readOnly)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	assert(numRenderTargets <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
	std::array<D3D12Resource *, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvResources{};
	for (uint i = 0; i < numRenderTargets; ++i)
	{
		assert(renderTargets[i].kind == ResourceKind::Texture);
		rtvResources[i] = static_cast<D3D12Resource *>(renderTargets[i].ptr);
	}

	assert(depthStencil.kind == ResourceKind::Unknown || depthStencil.kind == ResourceKind::Texture);
	D3D12Resource *dsvResource = static_cast<D3D12Resource *>(depthStencil.ptr);

	frameResources.graphicsCommandList->SetRenderTargets(numRenderTargets, rtvResources.data(), dsvResource, readOnly);
}

void D3D12RenderContext::ClearRenderTarget(Resource renderTarget, float clearColour[4])
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	assert(renderTarget.kind == ResourceKind::Texture);
	D3D12Resource *rtvResource = static_cast<D3D12Resource *>(renderTarget.ptr);

	frameResources.graphicsCommandList->ClearRenderTarget(rtvResource, clearColour);
}

void D3D12RenderContext::ClearDepthStencil(Resource depthStencil, float depth, uint8_t stencil)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	assert(depthStencil.kind == ResourceKind::Texture);
	D3D12Resource *dsvResource = static_cast<D3D12Resource *>(depthStencil.ptr);

	frameResources.graphicsCommandList->ClearDepthStencil(dsvResource, D3D12_CLEAR_FLAG_DEPTH, depth, stencil);
}

void D3D12RenderContext::ClearUAVBufferUint(ResourceHandle handle, uint32_t clearValue)
{
	auto it = resources_.find(handle);
	if (it == resources_.end() || !it->second.isBuffer)
	{
		throw std::runtime_error("ClearUAVBufferUint: invalid or non-buffer resource handle.");
	}
	ResourceEntry &entry = it->second;

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = static_cast<UINT>(entry.resourcePtr->GetResource()->GetDesc().Width / 4);
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

	const DescriptorHandle gpuClearHandle = resourceDescriptorHeap_->AllocateDescriptor();
	resourceDescriptorHeap_->CreateUAVAt(static_cast<uint32_t>(gpuClearHandle), entry.resourcePtr, uavDesc);
	cpuOnlyDescriptorHeap_->CreateUAVAt(static_cast<uint32_t>(gpuClearHandle), entry.resourcePtr, uavDesc);

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	const uint32_t values[4] = { clearValue, clearValue, clearValue, clearValue };
	frameResources.graphicsCommandList->ClearUnorderedAccessViewUint(resourceDescriptorHeap_.get(),
																	 gpuClearHandle,
																	 cpuOnlyDescriptorHeap_.get(),
																	 gpuClearHandle,
																	 entry.resourcePtr,
																	 values);
}

void D3D12RenderContext::SetViewport(const Viewport &viewport)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	const D3D12ViewPort d3dViewport = { static_cast<FLOAT>(viewport.x),
										static_cast<FLOAT>(viewport.y),
										static_cast<FLOAT>(viewport.width),
										static_cast<FLOAT>(viewport.height),
										viewport.minDepth,
										viewport.maxDepth };

	frameResources.graphicsCommandList->SetViewport(d3dViewport);
}

void D3D12RenderContext::SetScissorRect(const ScissorRect &scissorRect)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	const D3D12Rect d3dScissorRect = { static_cast<LONG>(scissorRect.x),
									   static_cast<LONG>(scissorRect.y),
									   static_cast<LONG>(scissorRect.width),
									   static_cast<LONG>(scissorRect.height) };

	frameResources.graphicsCommandList->SetScissorRect(d3dScissorRect);
}

void D3D12RenderContext::SetGraphics32BitConstants(const void *data, uint32_t num32BitValues)
{
	if (!currentGraphicsPipeline_)
	{
		throw std::runtime_error("SetGraphics32BitConstants: no graphics pipeline is currently bound.");
	}
	const uint32_t rootParameterIndex =
			currentGraphicsPipeline_->GetReflectedRootSignature().rootConstantParameterIndex;
	if (rootParameterIndex == kNoRootConstantParameter)
	{
		throw std::runtime_error(
				"SetGraphics32BitConstants: bound pipeline has no g_PushConstants/pushConstants buffer.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetGraphics32BitConstants(rootParameterIndex, num32BitValues, data, 0);
	MarkRootParameterSet(currentGraphicsPipeline_->GetReflectedRootSignature(), rootParameterIndex);
}

static D3D12_PRIMITIVE_TOPOLOGY TranslatePrimitiveTopology(PrimitiveTopology topology)
{
	switch (topology)
	{
		case PrimitiveTopology::PointList:
			return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		case PrimitiveTopology::LineList:
			return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		case PrimitiveTopology::LineStrip:
			return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		case PrimitiveTopology::TriangleList:
			return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		case PrimitiveTopology::TriangleStrip:
			return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		default:
			return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	}
}

void D3D12RenderContext::SetPrimitiveTopology(PrimitiveTopology topology)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetPrimitiveTopology(TranslatePrimitiveTopology(topology));
}

void D3D12RenderContext::Draw(uint vertexCount)
{
	ValidateRootParameters("Draw");
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void D3D12RenderContext::DispatchMesh(uint32_t threadGroupCountX,
									  uint32_t threadGroupCountY,
									  uint32_t threadGroupCountZ)
{
	ValidateRootParameters("DispatchMesh");
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->DispatchMesh(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

PipelineHandle D3D12RenderContext::CreateGraphicsPipeline(const std::string &shaderPath,
														  std::span<PipelineEntryPoint> entryPoints,
														  const GraphicsPipelineCreateInfo &pipelineInfo)
{
	const std::wstring wideShaderPath(shaderPath.begin(), shaderPath.end());

	std::vector<ShaderEntryPoint> shaderEntryPoints;
	shaderEntryPoints.reserve(entryPoints.size());
	for (const auto &entryPoint: entryPoints)
	{
		shaderEntryPoints.push_back({ std::wstring(entryPoint.name.begin(), entryPoint.name.end()),
									  TargetProfileForStage(entryPoint.stage) });
	}

	ShaderCompilationResult compileResult = shaderCompiler_->CompileShaderFromFile(wideShaderPath, shaderEntryPoints);
	if (!compileResult.success)
	{
		return InvalidPipelineHandle;
	}

	auto pipeline = device_->CreateGraphicsPipeline();
	pipeline->BuildRootSignatureFromShader(compileResult);
	pipeline->BuildGraphicsPipeline(pipelineInfo, compileResult);

	const PipelineHandle handle = nextPipelineHandle_++;
	graphicsPipelines_[handle] = std::move(pipeline);
	return handle;
}

void D3D12RenderContext::SetComputePipeline(PipelineHandle pipelineHandle)
{
	auto it = computePipelines_.find(pipelineHandle);
	if (it == computePipelines_.end())
	{
		throw std::runtime_error("Invalid pipeline handle.");
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetComputePipeline(it->second.get());
	currentComputePipeline_ = it->second.get();

	lastBoundPipelineKind_ = PipelineKind::Compute;
	BeginRootParameterTracking(computeRootParameters_, currentComputePipeline_->GetReflectedRootSignature());

	DefaultInitialiseDescriptorTables(
			currentComputePipeline_->GetReflectedRootSignature(),
			resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); });
}

void D3D12RenderContext::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
{
	ValidateRootParameters("Dispatch");
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

void D3D12RenderContext::SetCompute32BitConstants(const void *data, uint32_t num32BitValues)
{
	if (!currentComputePipeline_)
	{
		throw std::runtime_error("SetCompute32BitConstants: no compute pipeline is currently bound.");
	}
	const uint32_t rootParameterIndex = currentComputePipeline_->GetReflectedRootSignature().rootConstantParameterIndex;
	if (rootParameterIndex == kNoRootConstantParameter)
	{
		throw std::runtime_error(
				"SetCompute32BitConstants: bound pipeline has no g_PushConstants/pushConstants buffer.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetCompute32BitConstants(rootParameterIndex, num32BitValues, data, 0);
	MarkRootParameterSet(currentComputePipeline_->GetReflectedRootSignature(), rootParameterIndex);
}

void D3D12RenderContext::SetRaytracing32BitConstants(const void *data, uint32_t num32BitValues)
{
	if (!currentRaytracingPipeline_)
	{
		throw std::runtime_error("SetRaytracing32BitConstants: no raytracing pipeline is currently bound.");
	}
	const uint32_t rootParameterIndex =
			currentRaytracingPipeline_->GetReflectedRootSignature().rootConstantParameterIndex;
	if (rootParameterIndex == kNoRootConstantParameter)
	{
		throw std::runtime_error(
				"SetRaytracing32BitConstants: bound pipeline has no g_PushConstants/pushConstants buffer.");
	}
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetCompute32BitConstants(rootParameterIndex, num32BitValues, data, 0);
	MarkRootParameterSet(currentRaytracingPipeline_->GetReflectedRootSignature(), rootParameterIndex);
}

PipelineHandle D3D12RenderContext::CreateComputePipeline(const std::string &shaderPath,
														 const std::string &entryPoint,
														 const std::vector<std::string> &defines)
{
	const std::wstring wideShaderPath(shaderPath.begin(), shaderPath.end());
	const std::wstring wideEntryPoint(entryPoint.begin(), entryPoint.end());

	// CompileShaderFromFile turns any argument not starting with '-'/'/' into a preprocessor define.
	std::vector<std::wstring> arguments;
	arguments.reserve(defines.size());
	for (const std::string &define: defines)
	{
		arguments.emplace_back(define.begin(), define.end());
	}

	const ShaderEntryPoint shaderEntryPoint{ wideEntryPoint, L"cs_6_6" };
	ShaderCompilationResult compileResult =
			shaderCompiler_->CompileShaderFromFile(wideShaderPath,
												   std::span<const ShaderEntryPoint>(&shaderEntryPoint, 1),
												   arguments);
	if (!compileResult.success)
	{
		return InvalidPipelineHandle;
	}

	auto pipeline = device_->CreateComputePipeline();
	pipeline->BuildRootSignatureFromShader(compileResult);
	pipeline->BuildComputePipeline(compileResult);

	const PipelineHandle handle = nextPipelineHandle_++;
	computePipelines_[handle] = std::move(pipeline);
	return handle;
}

void D3D12RenderContext::SetRaytracingPipeline(PipelineHandle pipelineHandle)
{
	auto it = raytracingPipelines_.find(pipelineHandle);
	if (it == raytracingPipelines_.end())
	{
		throw std::runtime_error("Invalid pipeline handle.");
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetRaytracingPipeline(it->second.get());
	currentRaytracingPipeline_ = it->second.get();

	lastBoundPipelineKind_ = PipelineKind::Raytracing;
	BeginRootParameterTracking(raytracingRootParameters_, currentRaytracingPipeline_->GetReflectedRootSignature());

	// A raytracing pipeline binds its global root signature on the compute interface.
	DefaultInitialiseDescriptorTables(
			currentRaytracingPipeline_->GetReflectedRootSignature(),
			resourceDescriptorHeap_->GetGPUDescriptorHandleForHeapStart(),
			[&](uint32_t param, GPUDescriptorHandle handle)
			{ frameResources.graphicsCommandList->SetComputeDescriptorTable(param, handle); });
}

PipelineHandle D3D12RenderContext::CreateRaytracingPipeline(const std::string &shaderPath,
															const RaytracingPipelineDesc &pipelineDesc)
{
	const std::wstring wideShaderPath(shaderPath.begin(), shaderPath.end());

	// CompileShaderFromFile turns any argument not starting with '-'/'/' into a preprocessor define.
	std::vector<std::wstring> arguments;
	arguments.reserve(pipelineDesc.defines.size());
	for (const std::string &define: pipelineDesc.defines)
	{
		arguments.emplace_back(define.begin(), define.end());
	}

	// Slang only emits the entry points it is given, so every export (raygen/miss/hit/...) is
	// passed as an entry point of the library profile; they are all linked into one DXIL library.
	const std::wstring profile(pipelineDesc.profile.begin(), pipelineDesc.profile.end());
	std::vector<ShaderEntryPoint> shaderEntryPoints;
	shaderEntryPoints.reserve(pipelineDesc.exports.size());
	for (const auto &exportName: pipelineDesc.exports)
	{
		shaderEntryPoints.push_back({ std::wstring(exportName.begin(), exportName.end()), profile });
	}

	ShaderCompilationResult compileResult =
			shaderCompiler_->CompileShaderFromFile(wideShaderPath, shaderEntryPoints, arguments);
	if (!compileResult.success)
	{
		return InvalidPipelineHandle;
	}

	auto pipeline = device_->CreateRaytracingPipeline();
	pipeline->BuildRootSignatureFromShader(compileResult);

	RaytracingPipelineCreateInfo info;
	info.exports.reserve(pipelineDesc.exports.size());
	for (const auto &exportName: pipelineDesc.exports)
	{
		info.exports.emplace_back(exportName.begin(), exportName.end());
	}

	info.hitGroups.reserve(pipelineDesc.hitGroups.size());
	for (const auto &hitGroup: pipelineDesc.hitGroups)
	{
		RaytracingHitGroupDesc nativeHitGroup;
		nativeHitGroup.hitGroupExport = std::wstring(hitGroup.hitGroupExport.begin(), hitGroup.hitGroupExport.end());
		nativeHitGroup.closestHitExport =
				std::wstring(hitGroup.closestHitExport.begin(), hitGroup.closestHitExport.end());
		nativeHitGroup.anyHitExport = std::wstring(hitGroup.anyHitExport.begin(), hitGroup.anyHitExport.end());
		nativeHitGroup.intersectionExport =
				std::wstring(hitGroup.intersectionExport.begin(), hitGroup.intersectionExport.end());
		info.hitGroups.push_back(std::move(nativeHitGroup));
	}

	info.maxPayloadSizeInBytes = pipelineDesc.maxPayloadSizeInBytes;
	info.maxAttributeSizeInBytes = pipelineDesc.maxAttributeSizeInBytes;
	info.maxTraceRecursionDepth = pipelineDesc.maxTraceRecursionDepth;

	pipeline->BuildRaytracingPipeline(info, compileResult);

	const PipelineHandle handle = nextPipelineHandle_++;
	raytracingPipelines_[handle] = std::move(pipeline);
	return handle;
}

ShaderExecutionReorderingSupport D3D12RenderContext::GetShaderExecutionReorderingSupport() const
{
	return { device_->SupportsShaderExecutionReordering(), device_->ShaderExecutionReorderingActuallyReorders() };
}

ShaderTableHandle D3D12RenderContext::CreateShaderTable(PipelineHandle raytracingPipeline)
{
	auto it = raytracingPipelines_.find(raytracingPipeline);
	if (it == raytracingPipelines_.end())
	{
		throw std::runtime_error("CreateShaderTable: invalid raytracing pipeline handle.");
	}

	const ShaderTableHandle handle = nextShaderTableHandle_++;
	shaderTables_[handle] = std::make_unique<D3D12ShaderTable>(device_.get(), it->second.get());
	return handle;
}

void D3D12RenderContext::AddRayGenShaderRecord(ShaderTableHandle table, const std::string &exportName)
{
	auto it = shaderTables_.find(table);
	if (it == shaderTables_.end())
	{
		throw std::runtime_error("Invalid shader table handle.");
	}
	it->second->AddRayGenRecord(ShaderTableRecord{ std::wstring(exportName.begin(), exportName.end()), {} });
}

void D3D12RenderContext::AddMissShaderRecord(ShaderTableHandle table, const std::string &exportName)
{
	auto it = shaderTables_.find(table);
	if (it == shaderTables_.end())
	{
		throw std::runtime_error("Invalid shader table handle.");
	}
	it->second->AddMissRecord(ShaderTableRecord{ std::wstring(exportName.begin(), exportName.end()), {} });
}

void D3D12RenderContext::AddHitGroupShaderRecord(ShaderTableHandle table, const std::string &exportName)
{
	auto it = shaderTables_.find(table);
	if (it == shaderTables_.end())
	{
		throw std::runtime_error("Invalid shader table handle.");
	}
	it->second->AddHitGroupRecord(ShaderTableRecord{ std::wstring(exportName.begin(), exportName.end()), {} });
}

void D3D12RenderContext::BuildShaderTable(ShaderTableHandle table)
{
	auto it = shaderTables_.find(table);
	if (it == shaderTables_.end())
	{
		throw std::runtime_error("Invalid shader table handle.");
	}
	it->second->Build();
}

void D3D12RenderContext::DispatchRays(ShaderTableHandle table, uint32_t width, uint32_t height, uint32_t depth)
{
	ValidateRootParameters("DispatchRays");

	auto it = shaderTables_.find(table);
	if (it == shaderTables_.end())
	{
		throw std::runtime_error("Invalid shader table handle.");
	}

	D3D12ShaderTable &shaderTable = *it->second;

	D3D12_DISPATCH_RAYS_DESC desc = {};
	desc.RayGenerationShaderRecord = shaderTable.GetRayGenRange();
	desc.MissShaderTable = shaderTable.GetMissRange();
	desc.HitGroupTable = shaderTable.GetHitGroupRange();
	desc.Width = width;
	desc.Height = height;
	desc.Depth = depth;

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->DispatchRays(desc);
}

AccelerationStructureHandle D3D12RenderContext::CreateBLAS(const BLASGeometry &geometry)
{
	auto vertexIt = resources_.find(geometry.vertexBuffer);
	auto indexIt = resources_.find(geometry.indexBuffer);
	if (vertexIt == resources_.end() || indexIt == resources_.end())
	{
		throw std::runtime_error("CreateBLAS: invalid vertex or index buffer handle.");
	}

	BLASGeometryDesc nativeGeometry;
	nativeGeometry.vertexBufferVA =
			vertexIt->second.resourcePtr->GetResource()->GetGPUVirtualAddress() + geometry.vertexBufferOffset;
	nativeGeometry.vertexCount = geometry.vertexCount;
	nativeGeometry.vertexStride = geometry.vertexStride;
	nativeGeometry.indexBufferVA =
			indexIt->second.resourcePtr->GetResource()->GetGPUVirtualAddress() + geometry.indexBufferOffset;
	nativeGeometry.indexCount = geometry.indexCount;
	nativeGeometry.indexFormat = DXGI_FORMAT_R32_UINT;
	nativeGeometry.isOpaque = geometry.isOpaque;
	nativeGeometry.noDuplicateAnyHit = geometry.noDuplicateAnyHit;

	AccelerationStructureEntry entry;
	entry.kind = AccelerationStructureKind::BLAS;
	entry.blas = std::make_unique<D3D12BottomLevelAS>(device_.get(), nativeGeometry);

	const AccelerationStructureHandle handle = nextAccelerationStructureHandle_++;
	accelerationStructures_[handle] = std::move(entry);
	return handle;
}

AccelerationStructureHandle D3D12RenderContext::CreateTLAS(std::span<const TLASInstance> instances)
{
	std::vector<TLASInstanceDesc> nativeInstances;
	nativeInstances.reserve(instances.size());

	for (const TLASInstance &instance: instances)
	{
		auto blasIt = accelerationStructures_.find(instance.blas);
		if (blasIt == accelerationStructures_.end() || blasIt->second.kind != AccelerationStructureKind::BLAS)
		{
			throw std::runtime_error("CreateTLAS: invalid BLAS handle in instance list.");
		}

		TLASInstanceDesc nativeInstance;
		nativeInstance.blasAddress = blasIt->second.blas->GetGPUVirtualAddress();
		std::memcpy(nativeInstance.transform, instance.transform, sizeof(nativeInstance.transform));
		nativeInstance.instanceID = instance.instanceID;
		nativeInstance.instanceMask = instance.instanceMask;
		nativeInstance.hitGroupIndex = instance.hitGroupIndex;
		nativeInstances.push_back(nativeInstance);
	}

	AccelerationStructureEntry entry;
	entry.kind = AccelerationStructureKind::TLAS;
	entry.tlas = std::make_unique<D3D12TopLevelAS>(device_.get(), nativeInstances);

	const AccelerationStructureHandle handle = nextAccelerationStructureHandle_++;
	accelerationStructures_[handle] = std::move(entry);
	return handle;
}

void D3D12RenderContext::BuildAccelerationStructure(AccelerationStructureHandle handle)
{
	auto it = accelerationStructures_.find(handle);
	if (it == accelerationStructures_.end())
	{
		throw std::runtime_error("Invalid acceleration structure handle.");
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	AccelerationStructureEntry &entry = it->second;
	if (entry.kind == AccelerationStructureKind::BLAS)
	{
		entry.blas->Build(frameResources.graphicsCommandList.get());
	}
	else
	{
		entry.tlas->Build(frameResources.graphicsCommandList.get());
	}
}

ResourceHandle D3D12RenderContext::GetAccelerationStructureSRV(AccelerationStructureHandle handle)
{
	auto it = accelerationStructures_.find(handle);
	if (it == accelerationStructures_.end() || it->second.kind != AccelerationStructureKind::TLAS)
	{
		throw std::runtime_error("GetAccelerationStructureSRV: handle does not refer to a TLAS.");
	}

	AccelerationStructureEntry &entry = it->second;
	if (entry.srvResourceHandle != InvalidResourceHandle)
	{
		return entry.srvResourceHandle;
	}

	const DescriptorHandle descriptorIndex =
			resourceDescriptorHeap_->CreateAccelerationStructureSRV(entry.tlas->GetGPUVirtualAddress());

	const ResourceHandle resourceHandle = nextResourceHandle_++;
	ResourceEntry &resourceEntry = resources_[resourceHandle];
	resourceEntry.resourcePtr = entry.tlas->GetResource();
	resourceEntry.isBuffer = true;
	resourceEntry.descriptorIndex = descriptorIndex;
	resourceEntry.currentLayout = ResourceLayout::Undefined;
	resourceEntry.currentSync = ResourceSync::Raytracing;
	resourceEntry.currentAccess = ResourceAccess::RaytracingAccelerationStructureRead;

	entry.srvResourceHandle = resourceHandle;
	return resourceHandle;
}

void D3D12RenderContext::UAVBarrierForAccelerationStructures()
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->EmitUAVBarrier();
}

void D3D12RenderContext::DestroyAccelerationStructure(AccelerationStructureHandle handle)
{
	auto it = accelerationStructures_.find(handle);
	if (it == accelerationStructures_.end())
	{
		return;
	}

	if (it->second.srvResourceHandle != InvalidResourceHandle)
	{
		DestroyResource(it->second.srvResourceHandle);
	}

	accelerationStructures_.erase(it);
}

static D3D12_INDIRECT_ARGUMENT_TYPE TranslateIndirectCommandType(IndirectCommandType type)
{
	switch (type)
	{
		case IndirectCommandType::Dispatch:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		case IndirectCommandType::Draw:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
		case IndirectCommandType::DispatchMesh:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
	}
	throw std::runtime_error("Invalid IndirectCommandType.");
}

CommandSignatureHandle D3D12RenderContext::CreateCommandSignature(IndirectCommandType type,
																  uint32_t rootConstantsSizeInBytes,
																  PipelineHandle pipeline)
{
	ID3D12RootSignature *rootSignature = nullptr;
	if (rootConstantsSizeInBytes > 0)
	{
		if (auto it = computePipelines_.find(pipeline); it != computePipelines_.end())
		{
			rootSignature = it->second->GetNativeRootSignature().Get();
		}
		else if (auto git = graphicsPipelines_.find(pipeline); git != graphicsPipelines_.end())
		{
			rootSignature = git->second->GetNativeRootSignature().Get();
		}
		else
		{
			throw std::runtime_error("CreateCommandSignature: invalid pipeline handle for root-constants signature.");
		}
	}

	Microsoft::WRL::ComPtr<ID3D12CommandSignature> commandSignature;
	switch (TranslateIndirectCommandType(type))
	{
		case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
			commandSignature = device_->CreateDispatchIndirectCommandSignature(rootSignature, rootConstantsSizeInBytes);
			break;
		case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
			commandSignature = device_->CreateDrawIndirectCommandSignature(rootSignature, rootConstantsSizeInBytes);
			break;
		case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
			commandSignature =
					device_->CreateDispatchMeshIndirectCommandSignature(rootSignature, rootConstantsSizeInBytes);
			break;
		default:
			throw std::runtime_error("Invalid IndirectCommandType.");
	}

	const CommandSignatureHandle handle = nextCommandSignatureHandle_++;
	commandSignatures_[handle] = std::move(commandSignature);
	return handle;
}

void D3D12RenderContext::ExecuteIndirect(CommandSignatureHandle signature,
										 ResourceHandle argumentBuffer,
										 uint64_t argumentBufferOffset,
										 uint32_t maxCommandCount,
										 ResourceHandle countBuffer,
										 uint64_t countBufferOffset)
{
	ValidateRootParameters("ExecuteIndirect");

	auto signatureIt = commandSignatures_.find(signature);
	if (signatureIt == commandSignatures_.end())
	{
		throw std::runtime_error("Invalid command signature handle.");
	}

	auto argumentIt = resources_.find(argumentBuffer);
	if (argumentIt == resources_.end())
	{
		throw std::runtime_error("ExecuteIndirect: invalid argument buffer handle.");
	}

	ID3D12Resource *countResource = nullptr;
	if (countBuffer != InvalidResourceHandle)
	{
		auto countIt = resources_.find(countBuffer);
		if (countIt == resources_.end())
		{
			throw std::runtime_error("ExecuteIndirect: invalid count buffer handle.");
		}
		countResource = countIt->second.resourcePtr->GetResource();
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->ExecuteIndirect(signatureIt->second.Get(),
														maxCommandCount,
														argumentIt->second.resourcePtr->GetResource(),
														static_cast<UINT>(argumentBufferOffset),
														countResource,
														static_cast<UINT>(countBufferOffset));
}

static D3D12_RESOURCE_DIMENSION TranslateDimension(TextureDimension dimension)
{
	switch (dimension)
	{
		case TextureDimension::Texture1D:
			return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
		case TextureDimension::Texture3D:
			return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		case TextureDimension::Texture2D:
		case TextureDimension::TextureCube:
		default:
			return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	}
}

static D3D12_RESOURCE_FLAGS TranslateUsageFlags(ResourceUsageFlags usageFlags)
{
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	const uint32_t bits = static_cast<uint32_t>(usageFlags);

	if (bits & static_cast<uint32_t>(ResourceUsageFlags::RenderTarget))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (bits & static_cast<uint32_t>(ResourceUsageFlags::DepthStencil))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (bits & static_cast<uint32_t>(ResourceUsageFlags::UnorderedAccess))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	return flags;
}

DescriptorHandle D3D12RenderContext::CreateTextureViewDescriptor(D3D12Resource *resource, const TextureViewDesc &desc)
{
	const D3D12_RESOURCE_DESC resourceDesc = resource->GetResource()->GetDesc();
	const DXGI_FORMAT format =
			desc.format == Format::Unknown ? resourceDesc.Format : static_cast<DXGI_FORMAT>(desc.format);

	const bool isCube = desc.dimension == TextureDimension::TextureCube;
	const bool is3D = desc.dimension == TextureDimension::Texture3D;
	const bool is1D = desc.dimension == TextureDimension::Texture1D;

	const uint32_t mipLevels = resourceDesc.MipLevels;
	if (desc.baseMip >= mipLevels)
	{
		throw std::runtime_error("CreateTextureView: baseMip is out of range.");
	}
	const uint32_t remainingMips = mipLevels - desc.baseMip;
	const uint32_t mipCount = desc.mipCount == 0 ? remainingMips : desc.mipCount;

	// Slices of a 3D texture are depth slices; for everything else they are array slices.
	const uint32_t totalSlices = resourceDesc.DepthOrArraySize;
	if (desc.firstSlice >= totalSlices)
	{
		throw std::runtime_error("CreateTextureView: firstSlice is out of range.");
	}
	const uint32_t sliceCount = desc.sliceCount == 0 ? totalSlices - desc.firstSlice : desc.sliceCount;
	if (mipCount > remainingMips || desc.firstSlice + sliceCount > totalSlices)
	{
		throw std::runtime_error("CreateTextureView: mip or slice range exceeds the resource.");
	}

	switch (desc.viewType)
	{
		case ResourceViewType::ShaderResource:
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			// A typeless resource (e.g. a depth-stencil texture also sampled as a colour
			// texture) has no valid SRV format of its own; view it through its typed
			// equivalent instead.
			srvDesc.Format = format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : format;
			if (isCube)
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				srvDesc.TextureCube.MostDetailedMip = desc.baseMip;
				srvDesc.TextureCube.MipLevels = mipCount;
				if (desc.firstSlice != 0)
				{
					// A cube SRV over a later face group needs the array form.
					if (desc.firstSlice % kCubeFaceCount != 0)
					{
						throw std::runtime_error("CreateTextureView: cube firstSlice must be a multiple of 6.");
					}
					srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
					srvDesc.TextureCubeArray.MostDetailedMip = desc.baseMip;
					srvDesc.TextureCubeArray.MipLevels = mipCount;
					srvDesc.TextureCubeArray.First2DArrayFace = desc.firstSlice;
					srvDesc.TextureCubeArray.NumCubes = 1;
				}
			}
			else if (is3D)
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				srvDesc.Texture3D.MostDetailedMip = desc.baseMip;
				srvDesc.Texture3D.MipLevels = mipCount;
			}
			else if (is1D)
			{
				if (desc.asArray)
				{
					srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
					srvDesc.Texture1DArray.MostDetailedMip = desc.baseMip;
					srvDesc.Texture1DArray.MipLevels = mipCount;
					srvDesc.Texture1DArray.FirstArraySlice = desc.firstSlice;
					srvDesc.Texture1DArray.ArraySize = sliceCount;
				}
				else
				{
					srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
					srvDesc.Texture1D.MostDetailedMip = desc.baseMip;
					srvDesc.Texture1D.MipLevels = mipCount;
				}
			}
			else if (desc.asArray)
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MostDetailedMip = desc.baseMip;
				srvDesc.Texture2DArray.MipLevels = mipCount;
				srvDesc.Texture2DArray.FirstArraySlice = desc.firstSlice;
				srvDesc.Texture2DArray.ArraySize = sliceCount;
			}
			else
			{
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MostDetailedMip = desc.baseMip;
				srvDesc.Texture2D.MipLevels = mipCount;
			}
			return resourceDescriptorHeap_->CreateSRV(resource, srvDesc);
		}
		case ResourceViewType::UnorderedAccess:
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = format;
			if (isCube || (!is3D && !is1D && desc.asArray))
			{
				// A cube has no UAV dimension of its own; its faces are written as an array.
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				uavDesc.Texture2DArray.MipSlice = desc.baseMip;
				uavDesc.Texture2DArray.FirstArraySlice = desc.firstSlice;
				uavDesc.Texture2DArray.ArraySize = sliceCount;
			}
			else if (is3D)
			{
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
				uavDesc.Texture3D.MipSlice = desc.baseMip;
				uavDesc.Texture3D.FirstWSlice = desc.firstSlice;
				uavDesc.Texture3D.WSize = sliceCount;
			}
			else if (is1D)
			{
				if (desc.asArray)
				{
					uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
					uavDesc.Texture1DArray.MipSlice = desc.baseMip;
					uavDesc.Texture1DArray.FirstArraySlice = desc.firstSlice;
					uavDesc.Texture1DArray.ArraySize = sliceCount;
				}
				else
				{
					uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
					uavDesc.Texture1D.MipSlice = desc.baseMip;
				}
			}
			else
			{
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = desc.baseMip;
			}
			return resourceDescriptorHeap_->CreateUAV(resource, uavDesc);
		}
		case ResourceViewType::ConstantBuffer:
			throw std::runtime_error("CreateTextureView: ConstantBuffer views are not supported for textures.");
	}

	throw std::runtime_error("Invalid ResourceViewType.");
}

DescriptorHandle D3D12RenderContext::CreateBufferViewDescriptor(D3D12Resource *resource,
																ResourceViewType viewType,
																uint64_t sizeInBytes,
																uint32_t structureByteStride)
{
	switch (viewType)
	{
		case ResourceViewType::ShaderResource:
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			const uint32_t stride = structureByteStride != 0 ? structureByteStride : 4;
			srvDesc.Buffer.NumElements = static_cast<UINT>(sizeInBytes / stride);
			srvDesc.Buffer.StructureByteStride = structureByteStride;
			return resourceDescriptorHeap_->CreateSRV(resource, srvDesc);
		}
		case ResourceViewType::UnorderedAccess:
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			const uint32_t stride = structureByteStride != 0 ? structureByteStride : 4;
			uavDesc.Buffer.NumElements = static_cast<UINT>(sizeInBytes / stride);
			uavDesc.Buffer.StructureByteStride = structureByteStride;
			return resourceDescriptorHeap_->CreateUAV(resource, uavDesc);
		}
		case ResourceViewType::ConstantBuffer:
		{
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
			cbvDesc.BufferLocation = resource->GetResource()->GetGPUVirtualAddress();
			cbvDesc.SizeInBytes = static_cast<UINT>((sizeInBytes + 255) & ~255ull);
			return resourceDescriptorHeap_->CreateCBV(cbvDesc);
		}
	}

	throw std::runtime_error("Invalid ResourceViewType.");
}

ResourceHandle D3D12RenderContext::CreateTextureResourceAndView(const TextureDesc &desc,
																ResourceViewType viewType,
																D3D12Texture **outTexture)
{
	const bool isCube = desc.dimension == TextureDimension::TextureCube;

	D3D12_RESOURCE_DESC1 resourceDesc = {};
	resourceDesc.Dimension = TranslateDimension(desc.dimension);
	resourceDesc.Width = desc.width;
	resourceDesc.Height = desc.height;
	resourceDesc.DepthOrArraySize =
			isCube ? static_cast<UINT16>(kCubeFaceCount) : static_cast<UINT16>(desc.depthOrArraySize);
	resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
	resourceDesc.Format = static_cast<DXGI_FORMAT>(desc.format);
	resourceDesc.SampleDesc = { 1, 0 };
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = TranslateUsageFlags(desc.usageFlags);

	std::unique_ptr<D3D12Texture> texture =
			device_->CreateTexture(resourceDesc,
								   D3D12_HEAP_TYPE_DEFAULT,
								   static_cast<D3D12_BARRIER_LAYOUT>(desc.initialLayout));
	if (!desc.debugName.empty())
	{
		texture->SetName(desc.debugName);
	}

	TextureViewDesc viewDesc;
	viewDesc.format = desc.format;
	viewDesc.viewType = viewType;
	viewDesc.dimension = desc.dimension;
	const DescriptorHandle descriptorIndex = CreateTextureViewDescriptor(texture.get(), viewDesc);

	const ResourceHandle handle = nextResourceHandle_++;
	ResourceEntry &entry = resources_[handle];
	entry.resourcePtr = texture.get();
	*outTexture = texture.get();
	entry.ownedResource = std::move(texture);
	entry.isBuffer = false;
	entry.descriptorIndex = descriptorIndex;
	entry.currentLayout = ResourceLayout::Undefined;
	entry.currentSync = ResourceSync::None;
	entry.currentAccess = ResourceAccess::NoAccess;

	return handle;
}

ResourceHandle D3D12RenderContext::CreateBufferResourceAndView(const BufferDesc &desc,
															   ResourceViewType viewType,
															   D3D12Buffer **outBuffer)
{
	D3D12_RESOURCE_DESC1 resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = desc.sizeInBytes;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc = { 1, 0 };
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = TranslateUsageFlags(desc.usageFlags);

	std::unique_ptr<D3D12Buffer> buffer = device_->CreateBuffer(resourceDesc,
																D3D12_HEAP_TYPE_DEFAULT,
																static_cast<D3D12_BARRIER_LAYOUT>(desc.initialLayout));
	if (!desc.debugName.empty())
	{
		buffer->SetName(desc.debugName);
	}

	const DescriptorHandle descriptorIndex = CreateBufferViewDescriptor(buffer.get(), viewType, desc.sizeInBytes, desc.structureByteStride);

	const ResourceHandle handle = nextResourceHandle_++;
	ResourceEntry &entry = resources_[handle];
	entry.resourcePtr = buffer.get();
	*outBuffer = buffer.get();
	entry.ownedResource = std::move(buffer);
	entry.isBuffer = true;
	entry.descriptorIndex = descriptorIndex;
	entry.currentLayout = ResourceLayout::Undefined;
	entry.currentSync = ResourceSync::None;
	entry.currentAccess = ResourceAccess::NoAccess;

	return handle;
}

ResourceHandle D3D12RenderContext::CreateTexture(const TextureDesc &desc, ResourceViewType viewType)
{
	D3D12Texture *texture = nullptr;
	const ResourceHandle handle = CreateTextureResourceAndView(desc, viewType, &texture);
	return handle;
}

ResourceHandle D3D12RenderContext::CreateTextureView(ResourceHandle existingTexture, const TextureViewDesc &desc)
{
	auto it = resources_.find(existingTexture);
	if (it == resources_.end() || it->second.isBuffer)
	{
		throw std::runtime_error("CreateTextureView: invalid or non-texture resource handle.");
	}

	const DescriptorHandle descriptorIndex = CreateTextureViewDescriptor(it->second.resourcePtr, desc);

	const ResourceHandle handle = nextResourceHandle_++;
	ResourceEntry &entry = resources_[handle];
	entry.resourcePtr = it->second.resourcePtr; // non-owning: ownedResource stays null
	entry.isBuffer = false;
	entry.descriptorIndex = descriptorIndex;
	entry.currentLayout = ResourceLayout::Undefined;
	entry.currentSync = ResourceSync::None;
	entry.currentAccess = ResourceAccess::NoAccess;

	return handle;
}

ResourceHandle D3D12RenderContext::CreateBuffer(const BufferDesc &desc, ResourceViewType viewType)
{
	D3D12Buffer *buffer = nullptr;
	const ResourceHandle handle = CreateBufferResourceAndView(desc, viewType, &buffer);
	return handle;
}

std::vector<ResourceHandle> D3D12RenderContext::CreateBufferArray(std::span<const BufferDesc> descs,
																	ResourceViewType viewType)
{
	const uint32_t baseIndex =
			resourceDescriptorHeap_->AllocateContiguousDescriptors(static_cast<uint32_t>(descs.size()));

	std::vector<ResourceHandle> views;
	views.reserve(descs.size());

	for (size_t i = 0; i < descs.size(); ++i)
	{
		const BufferDesc &desc = descs[i];
		const uint32_t descriptorIndex = baseIndex + static_cast<uint32_t>(i);

		D3D12_RESOURCE_DESC1 resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = desc.sizeInBytes;
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc = { 1, 0 };
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		resourceDesc.Flags = TranslateUsageFlags(desc.usageFlags);

		std::unique_ptr<D3D12Buffer> buffer =
				device_->CreateBuffer(resourceDesc,
									  D3D12_HEAP_TYPE_DEFAULT,
									  static_cast<D3D12_BARRIER_LAYOUT>(desc.initialLayout));
		if (!desc.debugName.empty())
		{
			buffer->SetName(desc.debugName);
		}

		const uint32_t stride = desc.structureByteStride != 0 ? desc.structureByteStride : 4;
		if (viewType == ResourceViewType::UnorderedAccess)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = static_cast<UINT>(desc.sizeInBytes / stride);
			uavDesc.Buffer.StructureByteStride = desc.structureByteStride;
			resourceDescriptorHeap_->CreateUAVAt(descriptorIndex, buffer.get(), uavDesc);
		}
		else
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.NumElements = static_cast<UINT>(desc.sizeInBytes / stride);
			srvDesc.Buffer.StructureByteStride = desc.structureByteStride;
			resourceDescriptorHeap_->CreateSRVAt(descriptorIndex, buffer.get(), srvDesc);
		}

		const ResourceHandle handle = nextResourceHandle_++;
		ResourceEntry &entry = resources_[handle];
		entry.resourcePtr = buffer.get();
		entry.ownedResource = std::move(buffer);
		entry.isBuffer = true;
		entry.descriptorIndex = DescriptorHandle{ descriptorIndex };
		entry.currentLayout = ResourceLayout::Undefined;
		entry.currentSync = ResourceSync::None;
		entry.currentAccess = ResourceAccess::NoAccess;

		views.push_back(handle);
	}

	return views;
}

static uint64_t AlignUp(uint64_t size, uint64_t alignment)
{
	return (size + alignment - 1) & ~(alignment - 1);
}

UploadBatchHandle D3D12RenderContext::BeginUpload()
{
	UploadBatchEntry entry;
	entry.cmdList = device_->CreateCommandList(D3D12CommandListType::Copy);
	entry.fence = device_->CreateFence(0);
	entry.cmdList->SetName("UploadBatch_CopyList");
	entry.cmdList->BeginCommandList();

	const UploadBatchHandle handle = nextUploadBatchHandle_++;
	uploadBatches_[handle] = std::move(entry);
	return handle;
}

ResourceHandle D3D12RenderContext::CreateBuffer(const BufferDesc &desc,
												  ResourceViewType viewType,
												  const void *initialData,
												  UploadBatchHandle batch)
{
	D3D12Buffer *buffer = nullptr;
	const ResourceHandle handle = CreateBufferResourceAndView(desc, viewType, &buffer);

	if (initialData)
	{
		auto batchIt = uploadBatches_.find(batch);
		if (batchIt == uploadBatches_.end())
		{
			throw std::runtime_error("CreateBuffer: invalid upload batch handle.");
		}
		UploadBatchEntry &batchEntry = batchIt->second;

		const uint64_t alignedSize = AlignUp(desc.sizeInBytes, 256);
		D3D12_RESOURCE_DESC1 stagingDesc = {};
		stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		stagingDesc.Width = alignedSize;
		stagingDesc.Height = 1;
		stagingDesc.DepthOrArraySize = 1;
		stagingDesc.MipLevels = 1;
		stagingDesc.Format = DXGI_FORMAT_UNKNOWN;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		std::unique_ptr<D3D12Buffer> staging =
				device_->CreateBuffer(stagingDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_BARRIER_LAYOUT_UNDEFINED);
		staging->Map();
		std::memcpy(staging->GetMappedData(), initialData, desc.sizeInBytes);
		staging->Unmap();

		batchEntry.cmdList->CopyBufferRegion(buffer, 0, staging.get(), 0, desc.sizeInBytes);
		batchEntry.hasCopyWork = true;
		batchEntry.stagingBuffers.push_back(std::move(staging));
	}

	return handle;
}

ResourceHandle D3D12RenderContext::CreateTexture(const TextureDesc &desc,
												   ResourceViewType viewType,
												   std::span<const TextureMipData> mipData,
												   UploadBatchHandle batch)
{
	D3D12Texture *texture = nullptr;
	const ResourceHandle handle = CreateTextureResourceAndView(desc, viewType, &texture);

	if (!mipData.empty())
	{
		assert(mipData.size() == desc.mipLevels);

		auto batchIt = uploadBatches_.find(batch);
		if (batchIt == uploadBatches_.end())
		{
			throw std::runtime_error("CreateTexture: invalid upload batch handle.");
		}
		UploadBatchEntry &batchEntry = batchIt->second;

		const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(desc.format);

		std::vector<uint64_t> mipOffsets(mipData.size());
		std::vector<uint32_t> mipAlignedPitches(mipData.size());
		uint64_t totalStagingSize = 0;
		for (uint32_t m = 0; m < mipData.size(); ++m)
		{
			const uint32_t alignedPitch =
					static_cast<uint32_t>(AlignUp(mipData[m].rowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
			// Placed footprints must start on a 512-byte boundary, which a mip with an odd row count
			// (or a single row) would otherwise leave the next one off.
			totalStagingSize = AlignUp(totalStagingSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			mipOffsets[m] = totalStagingSize;
			mipAlignedPitches[m] = alignedPitch;
			const uint32_t depth = std::max(1u, mipData[m].depth);
			totalStagingSize += static_cast<uint64_t>(alignedPitch) * mipData[m].rowCount * depth;
		}

		D3D12_RESOURCE_DESC1 stagingDesc = {};
		stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		stagingDesc.Width = totalStagingSize;
		stagingDesc.Height = 1;
		stagingDesc.DepthOrArraySize = 1;
		stagingDesc.MipLevels = 1;
		stagingDesc.Format = DXGI_FORMAT_UNKNOWN;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		std::unique_ptr<D3D12Buffer> staging =
				device_->CreateBuffer(stagingDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_BARRIER_LAYOUT_UNDEFINED);
		staging->Map();
		uint8_t *stagingBase = static_cast<uint8_t *>(staging->GetMappedData());

		for (uint32_t m = 0; m < mipData.size(); ++m)
		{
			uint8_t *dst = stagingBase + mipOffsets[m];
			const uint8_t *src = static_cast<const uint8_t *>(mipData[m].pixels);
			const uint32_t depth = std::max(1u, mipData[m].depth);
			const uint32_t totalRows = mipData[m].rowCount * depth;
			for (uint32_t row = 0; row < totalRows; ++row)
				std::memcpy(dst + row * mipAlignedPitches[m], src + row * mipData[m].rowPitch, mipData[m].rowPitch);
		}
		staging->Unmap();

		// Copy-queue command lists only support COMMON as a texture layout (not COPY_DEST).
		batchEntry.cmdList->SetTextureBarrier(texture,
											  D3D12_BARRIER_LAYOUT_UNDEFINED,
											  D3D12_BARRIER_SYNC_NONE,
											  D3D12_BARRIER_ACCESS_NO_ACCESS,
											  D3D12_BARRIER_LAYOUT_COMMON,
											  D3D12_BARRIER_SYNC_COPY,
											  D3D12_BARRIER_ACCESS_COPY_DEST);

		ID3D12GraphicsCommandList *nativeCmdList = batchEntry.cmdList->GetNativeCommandList().Get();
		for (uint32_t m = 0; m < mipData.size(); ++m)
		{
			const uint32_t mipWidth = std::max(1u, desc.width >> m);
			const uint32_t mipHeight = std::max(1u, desc.height >> m);
			const uint32_t mipDepth = std::max(1u, mipData[m].depth);

			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = texture->GetResource();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = m;

			if (mipDepth > 1)
			{
				// One CopyTextureRegion per depth slice: some drivers mishandle a single
				// placed-footprint copy whose Depth > 1, so each slice is addressed individually via
				// dstZ/srcBox instead of relying on the footprint's Depth field.
				for (uint32_t z = 0; z < mipDepth; ++z)
				{
					D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
					footprint.Offset =
							mipOffsets[m] + static_cast<uint64_t>(z) * mipAlignedPitches[m] * mipData[m].rowCount;
					footprint.Footprint.Format = dxgiFormat;
					footprint.Footprint.Width = mipWidth;
					footprint.Footprint.Height = mipHeight;
					footprint.Footprint.Depth = 1;
					footprint.Footprint.RowPitch = mipAlignedPitches[m];

					D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
					srcLoc.pResource = staging->GetResource();
					srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					srcLoc.PlacedFootprint = footprint;

					nativeCmdList->CopyTextureRegion(&dstLoc, 0, 0, z, &srcLoc, nullptr);
				}
			}
			else
			{
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
				footprint.Offset = mipOffsets[m];
				footprint.Footprint.Format = dxgiFormat;
				footprint.Footprint.Width = mipWidth;
				footprint.Footprint.Height = mipHeight;
				footprint.Footprint.Depth = 1;
				footprint.Footprint.RowPitch = mipAlignedPitches[m];

				D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
				srcLoc.pResource = staging->GetResource();
				srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLoc.PlacedFootprint = footprint;

				nativeCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
			}
		}

		batchEntry.hasCopyWork = true;
		batchEntry.stagingBuffers.push_back(std::move(staging));

		// Copy queue's barrier bypasses normal TransitionResource bookkeeping - update tracked state directly.
		ResourceEntry &trackedEntry = resources_[handle];
		trackedEntry.currentLayout = ResourceLayout::Common;
		trackedEntry.currentSync = ResourceSync::Copy;
		trackedEntry.currentAccess = ResourceAccess::CopyDest;
	}

	return handle;
}

void D3D12RenderContext::EndUpload(UploadBatchHandle batch)
{
	auto batchIt = uploadBatches_.find(batch);
	if (batchIt == uploadBatches_.end())
	{
		throw std::runtime_error("EndUpload: invalid upload batch handle.");
	}
	UploadBatchEntry &entry = batchIt->second;

	entry.cmdList->EndCommandList();
	if (entry.hasCopyWork)
	{
		D3D12CommandList *cmds[] = { entry.cmdList.get() };
		const SubmitSyncInfo syncInfo = { .fence = entry.fence.get(), .signalValue = 1 };
		copyQueue_->ExecuteCommandLists(cmds, 1, syncInfo);
		entry.fence->Wait(1);
	}

	uploadBatches_.erase(batchIt);
}

ResourceHandle D3D12RenderContext::CreateUploadBuffer(const BufferDesc &desc,
														ResourceViewType viewType,
														const void *initialData)
{
	const uint64_t alignedSize = AlignUp(desc.sizeInBytes, 256);

	D3D12_RESOURCE_DESC1 resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = alignedSize;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc = { 1, 0 };
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	std::unique_ptr<D3D12Buffer> buffer =
			device_->CreateBuffer(resourceDesc, D3D12_HEAP_TYPE_GPU_UPLOAD, D3D12_BARRIER_LAYOUT_UNDEFINED);
	if (!desc.debugName.empty())
	{
		buffer->SetName(desc.debugName);
	}

	buffer->Map();
	if (initialData)
	{
		std::memcpy(buffer->GetMappedData(), initialData, desc.sizeInBytes);
	}

	const DescriptorHandle descriptorIndex = CreateBufferViewDescriptor(buffer.get(), viewType, desc.sizeInBytes, desc.structureByteStride);

	const ResourceHandle handle = nextResourceHandle_++;
	ResourceEntry &entry = resources_[handle];
	entry.mappedData = buffer->GetMappedData();
	entry.resourcePtr = buffer.get();
	entry.ownedResource = std::move(buffer);
	entry.isBuffer = true;
	entry.descriptorIndex = descriptorIndex;
	entry.currentLayout = ResourceLayout::Undefined;
	entry.currentSync = ResourceSync::None;
	entry.currentAccess = ResourceAccess::NoAccess;

	return handle;
}

void D3D12RenderContext::UpdateUploadBuffer(ResourceHandle handle,
											const void *data,
											uint64_t offset,
											uint64_t sizeInBytes)
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		throw std::runtime_error("Invalid resource handle.");
	}

	if (!it->second.mappedData)
	{
		throw std::runtime_error("UpdateUploadBuffer: resource is not a persistently-mapped upload buffer.");
	}

	std::memcpy(static_cast<uint8_t *>(it->second.mappedData) + offset, data, sizeInBytes);
}

void D3D12RenderContext::CopyTexture(ResourceHandle dstHandle, ResourceHandle srcHandle)
{
	auto dstIt = resources_.find(dstHandle);
	auto srcIt = resources_.find(srcHandle);
	if (dstIt == resources_.end() || srcIt == resources_.end())
	{
		throw std::runtime_error("CopyTexture: invalid resource handle.");
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->CopyResource(dstIt->second.resourcePtr, srcIt->second.resourcePtr);
}

void D3D12RenderContext::CopyBufferRegion(ResourceHandle dstHandle,
										  uint64_t dstOffset,
										  ResourceHandle srcHandle,
										  uint64_t srcOffset,
										  uint64_t sizeInBytes)
{
	auto dstIt = resources_.find(dstHandle);
	auto srcIt = resources_.find(srcHandle);
	if (dstIt == resources_.end() || srcIt == resources_.end())
	{
		throw std::runtime_error("CopyBufferRegion: invalid resource handle.");
	}

	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->CopyBufferRegion(dstIt->second.resourcePtr,
														 dstOffset,
														 srcIt->second.resourcePtr,
														 srcOffset,
														 sizeInBytes);
}

// std::unique_ptr<ITextureCompressor> D3D12RenderContext::CreateTextureCompressor()
// {
// 	return std::make_unique<D3D12TextureCompressor>(device_.get(), computeQueue_.get(), resourceDescriptorHeap_.get());
// }

// The GPU BC encoder (GPUEncoder) is not part of this build; callers must handle a null compressor.
std::unique_ptr<ITextureCompressor> D3D12RenderContext::CreateTextureCompressor()
{
	return nullptr;
}

ResourceHandle D3D12RenderContext::CreateConstantBuffer(uint64_t sizeInBytes, const std::string &debugName)
{
	const uint64_t alignedSize = (sizeInBytes + 255) & ~255ull;

	D3D12_RESOURCE_DESC1 resourceDesc = {};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = alignedSize;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
	resourceDesc.SampleDesc = { 1, 0 };
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	std::unique_ptr<D3D12Buffer> buffer =
			device_->CreateBuffer(resourceDesc, D3D12_HEAP_TYPE_GPU_UPLOAD, D3D12_BARRIER_LAYOUT_UNDEFINED);
	if (!debugName.empty())
	{
		buffer->SetName(debugName);
	}

	buffer->Map();

	const DescriptorHandle descriptorIndex = CreateBufferViewDescriptor(buffer.get(), ResourceViewType::ConstantBuffer, alignedSize, 0);

	const ResourceHandle handle = nextResourceHandle_++;
	ResourceEntry &entry = resources_[handle];
	entry.mappedData = buffer->GetMappedData();
	entry.resourcePtr = buffer.get();
	entry.ownedResource = std::move(buffer);
	entry.isBuffer = true;
	entry.descriptorIndex = descriptorIndex;
	entry.currentLayout = ResourceLayout::Undefined;
	entry.currentSync = ResourceSync::None;
	entry.currentAccess = ResourceAccess::NoAccess;

	return handle;
}

void D3D12RenderContext::UpdateConstantBuffer(ResourceHandle handle, const void *data, uint64_t sizeInBytes)
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		throw std::runtime_error("Invalid resource handle.");
	}

	if (!it->second.mappedData)
	{
		throw std::runtime_error("UpdateConstantBuffer: resource is not a persistently-mapped constant buffer.");
	}

	std::memcpy(it->second.mappedData, data, sizeInBytes);
}

ResourceViewInfo D3D12RenderContext::GetResourceViewInfo(ResourceHandle handle) const
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		throw std::runtime_error("Invalid resource handle.");
	}

	const ResourceEntry &entry = it->second;
	const CPUDescriptorHandle cpuHandle = resourceDescriptorHeap_->GetCPUDescriptorHandle(entry.descriptorIndex);
	const GPUDescriptorHandle gpuHandle = resourceDescriptorHeap_->GetGPUDescriptorHandle(entry.descriptorIndex);

	ResourceViewInfo info;
	info.handle = handle;
	info.descriptorIndex = entry.descriptorIndex;
	info.cpuDescriptorHandle = cpuHandle.ptr;
	info.gpuDescriptorHandle = gpuHandle.ptr;
	return info;
}

Resource D3D12RenderContext::GetResource(ResourceHandle handle) const
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		throw std::runtime_error("Invalid resource handle.");
	}

	return Resource{ it->second.resourcePtr, it->second.isBuffer ? ResourceKind::Buffer : ResourceKind::Texture };
}

void D3D12RenderContext::TransitionResource(ResourceHandle handle,
											ResourceLayout newLayout,
											ResourceSync newSync,
											ResourceAccess newAccess)
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		throw std::runtime_error("Invalid resource handle.");
	}

	ResourceEntry &entry = it->second;
	auto &frameResources = perFrameResources_[currentFrameIndex_];

	if (entry.isBuffer)
	{
		frameResources.graphicsCommandList->SetBufferBarrier(entry.resourcePtr,
															 static_cast<D3D12_BARRIER_SYNC>(entry.currentSync),
															 static_cast<D3D12_BARRIER_ACCESS>(entry.currentAccess),
															 static_cast<D3D12_BARRIER_SYNC>(newSync),
															 static_cast<D3D12_BARRIER_ACCESS>(newAccess));
	}
	else
	{
		frameResources.graphicsCommandList->SetTextureBarrier(entry.resourcePtr,
															  static_cast<D3D12_BARRIER_LAYOUT>(entry.currentLayout),
															  static_cast<D3D12_BARRIER_SYNC>(entry.currentSync),
															  static_cast<D3D12_BARRIER_ACCESS>(entry.currentAccess),
															  static_cast<D3D12_BARRIER_LAYOUT>(newLayout),
															  static_cast<D3D12_BARRIER_SYNC>(newSync),
															  static_cast<D3D12_BARRIER_ACCESS>(newAccess));
	}

	entry.currentLayout = newLayout;
	entry.currentSync = newSync;
	entry.currentAccess = newAccess;
}

void D3D12RenderContext::GlobalBarrier(ResourceSync syncBefore,
									   ResourceAccess accessBefore,
									   ResourceSync syncAfter,
									   ResourceAccess accessAfter)
{
	auto &frameResources = perFrameResources_[currentFrameIndex_];
	frameResources.graphicsCommandList->SetGlobalBarrier(static_cast<D3D12_BARRIER_SYNC>(syncBefore),
														 static_cast<D3D12_BARRIER_ACCESS>(accessBefore),
														 static_cast<D3D12_BARRIER_SYNC>(syncAfter),
														 static_cast<D3D12_BARRIER_ACCESS>(accessAfter));
}

void D3D12RenderContext::DestroyResource(ResourceHandle handle)
{
	auto it = resources_.find(handle);
	if (it == resources_.end())
	{
		return;
	}

	resourceDescriptorHeap_->FreeResource(it->second.descriptorIndex);
	resources_.erase(it);
}

void D3D12RenderContext::RegisterSwapchainBackBuffers()
{
	for (ResourceHandle handle: backBufferHandles_)
	{
		DestroyResource(handle);
	}
	backBufferHandles_.clear();

	const auto backBufferFormat = static_cast<Format>(swapchain_->GetBackBufferFormat());

	auto &backBuffers = swapchain_->GetBackBuffers();
	backBufferHandles_.reserve(backBuffers.size());

	for (auto &backBuffer: backBuffers)
	{
		TextureViewDesc viewDesc;
		viewDesc.format = backBufferFormat;
		const DescriptorHandle descriptorIndex = CreateTextureViewDescriptor(backBuffer.get(), viewDesc);

		const ResourceHandle handle = nextResourceHandle_++;
		ResourceEntry &entry = resources_[handle];
		entry.resourcePtr = backBuffer.get(); // non-owning: D3D12Swapchain retains ownership
		entry.isBuffer = false;
		entry.descriptorIndex = descriptorIndex;
		entry.currentLayout = ResourceLayout::Present;
		entry.currentSync = ResourceSync::All;
		entry.currentAccess = ResourceAccess::Common;

		backBufferHandles_.push_back(handle);
	}
}
