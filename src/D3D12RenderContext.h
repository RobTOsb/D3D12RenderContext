#pragma once

#include "D3D12RHI.h"
#include "RenderContext.h"

#include <set>
#include <utility>


class Window;
class D3D12ImguiRenderer;

class D3D12RenderContext final : public RenderContext
{
public:
	D3D12RenderContext(Window *window,
					   uint framesInFlight = 3,
					   uint swapchainImageCount = 3,
					   bool useSoftwareAdapter = false,
					   bool useHDR = false);
	~D3D12RenderContext() override;

	D3D12Device *GetDevice() const
	{
		return device_.get();
	}

	D3D12CommandQueue *GetGraphicsQueue() const
	{
		return graphicsQueue_.get();
	}

	D3D12CommandQueue *GetComputeQueue() const
	{
		return computeQueue_.get();
	}

	D3D12DescriptorHeap *GetDescriptorHeap() const
	{
		return resourceDescriptorHeap_.get();
	}

	ID3D12GraphicsCommandList *GetNativeGraphicsCommandList() const
	{
		return perFrameResources_[currentFrameIndex_].graphicsCommandList->GetNativeCommandList().Get();
	}

	ID3D12Resource *GetNativeResource(ResourceHandle handle) const
	{
		return GetResource(handle).kind == ResourceKind::Unknown
					   ? nullptr
					   : static_cast<D3D12Resource *>(GetResource(handle).ptr)->GetResource();
	}

	Resource GetCurrentBackBuffer() override
	{
		return Resource{ swapchain_->GetCurrentBackBuffer(), ResourceKind::Texture };
	}

	ResourceHandle GetCurrentBackBufferHandle() const override
	{
		return backBufferHandles_[swapchain_->GetCurrentBackBufferIndex()];
	}

	Format GetBackBufferFormat() const override
	{
		return static_cast<Format>(swapchain_->GetBackBufferFormat());
	}

	void CreatePerFrameResources() override;
	void BeginFrame() override;
	void EndFrame() override;
	void BeginGraphicsCommandList() override;
	void EndGraphicsCommandList() override;
	void WaitForGPUIdle() override;
	void Present() override;

	void InitialiseImgui(void *hwnd) override;
	void ImguiNewFrame() override;
	void ImguiRender() override;

	void SetGraphicsPipeline(PipelineHandle pipelineHandle) override;
	void SetDescriptorHeaps() override;
	void BeginEvent(const std::string &label) override;
	void EndEvent() override;

	void BeginProfileScope(const std::string &name) override;
	void EndProfileScope() override;
	const GPUProfileScopeStats *GetProfileScopeStats(const std::string &name) const override;
	std::vector<GPUProfileScopeStats> GetAllProfileScopeStats() const override;
	void ResetProfiler() override;
	void SetSRV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetUAV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetSRVByName(const std::string &name, ResourceHandle resource) override;
	void SetUAVByName(const std::string &name, ResourceHandle resource) override;
	void SetConstantBufferByName(const std::string &name, ResourceHandle resource) override;

	void Draw(uint vertexCount = 3) override;
	void DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;

	PipelineHandle CreateComputePipeline(const std::string &shaderPath,
										 const std::string &entryPoint,
										 const std::vector<std::string> &defines = {}) override;
	void SetComputePipeline(PipelineHandle pipelineHandle) override;
	void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
	void SetCompute32BitConstants(const void *data, uint32_t num32BitValues) override;
	void SetComputeSRV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetComputeUAV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetComputeConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetComputeSRVByName(const std::string &name, ResourceHandle resource) override;
	void SetComputeUAVByName(const std::string &name, ResourceHandle resource) override;
	void SetComputeConstantBufferByName(const std::string &name, ResourceHandle resource) override;
	void SetComputeScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes) override;

	PipelineHandle CreateRaytracingPipeline(const std::string &shaderPath,
											const RaytracingPipelineDesc &pipelineDesc) override;
	void SetRaytracingPipeline(PipelineHandle pipelineHandle) override;
	void SetRaytracing32BitConstants(const void *data, uint32_t num32BitValues) override;
	void SetRaytracingSRV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetRaytracingUAV(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetRaytracingConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) override;
	void SetRaytracingSRVByName(const std::string &name, ResourceHandle resource) override;
	void SetRaytracingUAVByName(const std::string &name, ResourceHandle resource) override;
	void SetRaytracingConstantBufferByName(const std::string &name, ResourceHandle resource) override;
	void SetRaytracingScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes) override;

	ShaderExecutionReorderingSupport GetShaderExecutionReorderingSupport() const override;

	GPUMemoryStats GetMemoryStats() const override;

	ShaderTableHandle CreateShaderTable(PipelineHandle raytracingPipeline) override;
	void AddRayGenShaderRecord(ShaderTableHandle table, const std::string &exportName) override;
	void AddMissShaderRecord(ShaderTableHandle table, const std::string &exportName) override;
	void AddHitGroupShaderRecord(ShaderTableHandle table, const std::string &exportName) override;
	void BuildShaderTable(ShaderTableHandle table) override;
	void DispatchRays(ShaderTableHandle table, uint32_t width, uint32_t height, uint32_t depth) override;

	AccelerationStructureHandle CreateBLAS(const BLASGeometry &geometry) override;
	AccelerationStructureHandle CreateTLAS(std::span<const TLASInstance> instances) override;
	void BuildAccelerationStructure(AccelerationStructureHandle handle) override;
	void BuildCompactedBLASes(std::span<const AccelerationStructureHandle> handles) override;
	ResourceHandle GetAccelerationStructureSRV(AccelerationStructureHandle handle) override;
	void DestroyAccelerationStructure(AccelerationStructureHandle handle) override;
	void UAVBarrierForAccelerationStructures() override;

	CommandSignatureHandle CreateCommandSignature(IndirectCommandType type,
												  uint32_t rootConstantsSizeInBytes,
												  PipelineHandle pipeline) override;
	void ExecuteIndirect(CommandSignatureHandle signature,
						 ResourceHandle argumentBuffer,
						 uint64_t argumentBufferOffset,
						 uint32_t maxCommandCount,
						 ResourceHandle countBuffer = InvalidResourceHandle,
						 uint64_t countBufferOffset = 0) override;

	void SetPrimitiveTopology(PrimitiveTopology topology = PrimitiveTopology::TriangleList) override;
	void ClearRenderTarget(Resource renderTarget, float clearColour[4]) override;
	void ClearDepthStencil(Resource depthStencil, float depth = 1.0f, uint8_t stencil = 0) override;
	void ClearUAVBufferUint(ResourceHandle handle, uint32_t clearValue = 0) override;
	void SetRenderTargets(const Resource *renderTargets,
						  uint numRenderTargets,
						  Resource depthStencil = {},
						  bool readOnly = false) override;
	void SetViewport(const Viewport &viewport) override;
	void SetScissorRect(const ScissorRect &scissorRect) override;
	void SetGraphics32BitConstants(const void *data, uint32_t num32BitValues) override;

	PipelineHandle CreateGraphicsPipeline(const std::string &shaderPath,
										  std::span<PipelineEntryPoint> entryPoints,
										  const GraphicsPipelineCreateInfo &pipelineInfo) override;

	ResourceHandle CreateTexture(const TextureDesc &desc, ResourceViewType viewType) override;
	ResourceHandle CreateBuffer(const BufferDesc &desc, ResourceViewType viewType) override;
	ResourceHandle CreateTextureView(ResourceHandle existingTexture, const TextureViewDesc &desc) override;

	std::vector<ResourceHandle> CreateBufferArray(std::span<const BufferDesc> descs,
													ResourceViewType viewType) override;
	void SetComputeUAVTableByName(const std::string &name, ResourceHandle firstInArray) override;
	void SetComputeSRVTableByName(const std::string &name, ResourceHandle firstInArray) override;
	void SetUAVTableByName(const std::string &name, ResourceHandle firstInArray) override;

	UploadBatchHandle BeginUpload() override;
	void EndUpload(UploadBatchHandle batch) override;
	ResourceHandle CreateBuffer(const BufferDesc &desc,
								  ResourceViewType viewType,
								  const void *initialData,
								  UploadBatchHandle batch) override;
	ResourceHandle CreateTexture(const TextureDesc &desc,
								   ResourceViewType viewType,
								   std::span<const TextureMipData> mipData,
								   UploadBatchHandle batch) override;

	ResourceHandle CreateConstantBuffer(uint64_t sizeInBytes, const std::string &debugName = {}) override;
	void UpdateConstantBuffer(ResourceHandle handle, const void *data, uint64_t sizeInBytes) override;

	ResourceHandle CreateUploadBuffer(const BufferDesc &desc,
										ResourceViewType viewType,
										const void *initialData = nullptr) override;
	void UpdateUploadBuffer(ResourceHandle handle, const void *data, uint64_t offset, uint64_t sizeInBytes) override;
	void CopyTexture(ResourceHandle dstHandle, ResourceHandle srcHandle) override;
	void CopyBufferRegion(ResourceHandle dstHandle,
						  uint64_t dstOffset,
						  ResourceHandle srcHandle,
						  uint64_t srcOffset,
						  uint64_t sizeInBytes) override;
	std::unique_ptr<ITextureCompressor> CreateTextureCompressor() override;
	ResourceViewInfo GetResourceViewInfo(ResourceHandle handle) const override;
	Resource GetResource(ResourceHandle handle) const override;
	void TransitionResource(ResourceHandle handle,
							ResourceLayout newLayout,
							ResourceSync newSync,
							ResourceAccess newAccess) override;
	void GlobalBarrier(ResourceSync syncBefore,
					   ResourceAccess accessBefore,
					   ResourceSync syncAfter,
					   ResourceAccess accessAfter) override;
	void DestroyResource(ResourceHandle handle) override;

	void MarkRootParameterSet(const ReflectedRootSignature &rootSignature, uint32_t rootParameterIndex);

private:
	DescriptorHandle CreateTextureViewDescriptor(D3D12Resource *resource, const TextureViewDesc &desc);
	DescriptorHandle CreateBufferViewDescriptor(D3D12Resource *resource,
											ResourceViewType viewType,
											uint64_t sizeInBytes,
											uint32_t structureByteStride);

	// Registers non-owning ResourceEntry per swapchain back buffer.
	void RegisterSwapchainBackBuffers();

	ResourceHandle CreateTextureResourceAndView(const TextureDesc &desc,
												ResourceViewType viewType,
												D3D12Texture **outTexture);
	ResourceHandle CreateBufferResourceAndView(const BufferDesc &desc,
											   ResourceViewType viewType,
											   D3D12Buffer **outBuffer);

	std::unique_ptr<D3D12Device> device_;
	std::unique_ptr<D3D12CommandQueue> graphicsQueue_;
	std::unique_ptr<D3D12CommandQueue> copyQueue_;
	std::unique_ptr<D3D12CommandQueue> computeQueue_;
	std::unique_ptr<D3D12Swapchain> swapchain_;
	std::unique_ptr<D3D12DescriptorHeap> resourceDescriptorHeap_;
	std::unique_ptr<D3D12ImguiRenderer> imguiRenderer_;
	std::unique_ptr<D3D12GPUProfiler> gpuProfiler_; // created with the per-frame resources
	std::unique_ptr<D3D12DescriptorHeap> cpuOnlyDescriptorHeap_;
	std::unique_ptr<SlangShaderCompiler> shaderCompiler_;

	struct UploadBatchEntry
	{
		std::unique_ptr<D3D12CommandList> cmdList; // Copy-type command list, one per batch
		std::unique_ptr<D3D12Fence> fence;
		std::vector<std::unique_ptr<D3D12Resource>> stagingBuffers; // kept alive until the wait in EndUpload completes
		bool hasCopyWork = false;
	};

	std::unordered_map<UploadBatchHandle, UploadBatchEntry> uploadBatches_;
	UploadBatchHandle nextUploadBatchHandle_ = 0;

	std::unordered_map<PipelineHandle, std::unique_ptr<D3D12GraphicsPipeline>> graphicsPipelines_;
	std::unordered_map<PipelineHandle, std::unique_ptr<D3D12ComputePipeline>> computePipelines_;
	std::unordered_map<PipelineHandle, std::unique_ptr<D3D12RaytracingPipeline>> raytracingPipelines_;
	PipelineHandle nextPipelineHandle_ = 0;
	D3D12GraphicsPipeline *currentGraphicsPipeline_ = nullptr;
	D3D12ComputePipeline *currentComputePipeline_ = nullptr;
	D3D12RaytracingPipeline *currentRaytracingPipeline_ = nullptr;

	enum class PipelineKind
	{
		None,
		Graphics,
		Compute,
		Raytracing
	};
	struct RootParameterTracker
	{
		const ReflectedRootSignature *rootSignature = nullptr;
		// A root signature costs at most 64 DWORDs and every parameter costs at least one, so a
		// parameter index never reaches 64 and a 64-bit mask always suffices.
		uint64_t requiredMask = 0;
		uint64_t setMask = 0;
	};

	// Records which parameters of rootSignature must be set by hand, and clears the "set" state.
	void BeginRootParameterTracking(RootParameterTracker &tracker, const ReflectedRootSignature &rootSignature);
	// Reports any parameter of the most recently bound pipeline that was never set.
	void ValidateRootParameters(const char *callSite);

	RootParameterTracker graphicsRootParameters_;
	RootParameterTracker computeRootParameters_;
	RootParameterTracker raytracingRootParameters_;
	PipelineKind lastBoundPipelineKind_ = PipelineKind::None;
	std::set<std::pair<const ReflectedRootSignature *, uint32_t>> reportedRootParameters_;

	struct ScalarCBKey
	{
		const void *pipeline;
		std::string cbufferName;
		bool operator==(const ScalarCBKey &other) const
		{
			return pipeline == other.pipeline && cbufferName == other.cbufferName;
		}
	};
	struct ScalarCBKeyHash
	{
		size_t operator()(const ScalarCBKey &key) const
		{
			return std::hash<const void *>()(key.pipeline) ^ (std::hash<std::string>()(key.cbufferName) << 1);
		}
	};
	std::unordered_map<ScalarCBKey, ResourceHandle, ScalarCBKeyHash> scalarConstantBuffers_;
	ResourceHandle GetOrCreateScalarConstantBuffer(const void *pipeline,
												   const std::string &cbufferName,
												   uint32_t sizeInBytes);

	std::unordered_map<ShaderTableHandle, std::unique_ptr<D3D12ShaderTable>> shaderTables_;
	ShaderTableHandle nextShaderTableHandle_ = 0;

	enum class AccelerationStructureKind
	{
		BLAS,
		TLAS
	};
	struct AccelerationStructureEntry
	{
		AccelerationStructureKind kind = AccelerationStructureKind::BLAS;
		std::unique_ptr<D3D12BottomLevelAS> blas;
		std::unique_ptr<D3D12TopLevelAS> tlas;
		ResourceHandle srvResourceHandle = InvalidResourceHandle; // lazily registered in resources_
	};
	std::unordered_map<AccelerationStructureHandle, AccelerationStructureEntry> accelerationStructures_;
	AccelerationStructureHandle nextAccelerationStructureHandle_ = 0;

	std::unordered_map<CommandSignatureHandle, Microsoft::WRL::ComPtr<ID3D12CommandSignature>> commandSignatures_;
	CommandSignatureHandle nextCommandSignatureHandle_ = 0;

	struct ResourceEntry
	{
		// Null for non-owned entries (e.g. swapchain back buffers).
		std::unique_ptr<D3D12Resource> ownedResource;
		D3D12Resource *resourcePtr = nullptr;
		bool isBuffer = false;
		void *mappedData = nullptr; // non-null for persistently-mapped constant buffers
		DescriptorHandle descriptorIndex = InvalidDescriptorHandle;
		ResourceLayout currentLayout = ResourceLayout::Undefined;
		ResourceSync currentSync = ResourceSync::None;
		ResourceAccess currentAccess = ResourceAccess::Common;
	};

	std::unordered_map<ResourceHandle, ResourceEntry> resources_;
	ResourceHandle nextResourceHandle_ = 0;

	std::vector<ResourceHandle> backBufferHandles_;

	struct PerFrameResources
	{
		std::unique_ptr<D3D12CommandAllocator> graphicsCommandAllocator;
		std::unique_ptr<D3D12CommandList> graphicsCommandList;
		std::unique_ptr<D3D12Fence> fence;
		uint64_t fenceValue = 0;
	};

	std::vector<PerFrameResources> perFrameResources_;
};
