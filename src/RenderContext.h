#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include "Typedefs.h"

#include "D3D12GPUProfiler.h"
#include "D3D12Pipeline.h"
#include "DirectXTex.h"

enum class RenderAPI
{
	D3D12,
	Vulkan,
	OpenGL,
	Metal
};

class Window;

using PipelineHandle = int16;

static constexpr PipelineHandle InvalidPipelineHandle = -1;

enum class PipelineShaderStage
{
	Vertex,
	Pixel,
	Amplification,
	Mesh
};

struct PipelineEntryPoint
{
	std::string name;
	PipelineShaderStage stage;
};

struct Viewport
{
	uint x;
	uint y;
	uint width;
	uint height;
	float minDepth;
	float maxDepth;
};

struct ScissorRect
{
	uint x;
	uint y;
	uint width;
	uint height;
};

enum class PrimitiveTopology
{
	TriangleList,
	TriangleStrip,
	LineList,
	LineStrip,
	PointList
};

// Mirror D3D12 enhanced barrier enums exactly
enum class ResourceLayout : uint32_t
{
	Undefined = 0xffffffff,
	Common = 0,
	Present = 0,
	GenericRead = 1,
	RenderTarget = 2,
	UnorderedAccess = 3,
	DepthStencilWrite = 4,
	DepthStencilRead = 5,
	ShaderResource = 6,
	CopySource = 7,
	CopyDest = 8,
	ResolveSource = 9,
	ResolveDest = 10,
	ShadingRateSource = 11,
	VideoDecodeRead = 12,
	VideoDecodeWrite = 13,
	VideoProcessRead = 14,
	VideoProcessWrite = 15,
	VideoEncodeRead = 16,
	VideoEncodeWrite = 17,
	DirectQueueCommon = 18,
	DirectQueueGenericRead = 19,
	DirectQueueUnorderedAccess = 20,
	DirectQueueShaderResource = 21,
	DirectQueueCopySource = 22,
	DirectQueueCopyDest = 23,
	ComputeQueueCommon = 24,
	ComputeQueueGenericRead = 25,
	ComputeQueueUnorderedAccess = 26,
	ComputeQueueShaderResource = 27,
	ComputeQueueCopySource = 28,
	ComputeQueueCopyDest = 29,
	DirectQueueGenericReadComputeQueueAccessible = 31
};

enum class ResourceSync : uint32_t
{
	None = 0,
	All = 0x1,
	Draw = 0x2,
	IndexInput = 0x4,
	VertexShading = 0x8,
	PixelShading = 0x10,
	DepthStencil = 0x20,
	RenderTarget = 0x40,
	ComputeShading = 0x80,
	Raytracing = 0x100,
	Copy = 0x200,
	Resolve = 0x400,
	ExecuteIndirect = 0x800,
	Predication = 0x800,
	AllShading = 0x1000,
	NonPixelShading = 0x2000,
	EmitRaytracingAccelerationStructurePostbuildInfo = 0x4000,
	ClearUnorderedAccessView = 0x8000,
	VideoDecode = 0x100000,
	VideoProcess = 0x200000,
	VideoEncode = 0x400000,
	BuildRaytracingAccelerationStructure = 0x800000,
	CopyRaytracingAccelerationStructure = 0x1000000,
	Split = 0x80000000
};

enum class ResourceAccess : uint32_t
{
	Common = 0,
	VertexBuffer = 0x1,
	ConstantBuffer = 0x2,
	IndexBuffer = 0x4,
	RenderTarget = 0x8,
	UnorderedAccess = 0x10,
	DepthStencilWrite = 0x20,
	DepthStencilRead = 0x40,
	ShaderResource = 0x80,
	StreamOutput = 0x100,
	IndirectArgument = 0x200,
	Predication = 0x200,
	CopyDest = 0x400,
	CopySource = 0x800,
	ResolveDest = 0x1000,
	ResolveSource = 0x2000,
	RaytracingAccelerationStructureRead = 0x4000,
	RaytracingAccelerationStructureWrite = 0x8000,
	ShadingRateSource = 0x10000,
	VideoDecodeRead = 0x20000,
	VideoDecodeWrite = 0x40000,
	VideoProcessRead = 0x80000,
	VideoProcessWrite = 0x100000,
	VideoEncodeRead = 0x200000,
	VideoEncodeWrite = 0x400000,
	NoAccess = 0x80000000
};

using ResourceHandle = uint32_t;
static constexpr ResourceHandle InvalidResourceHandle = UINT32_MAX;

enum class ResourceViewType
{
	ShaderResource, // SRV
	UnorderedAccess, // UAV
	ConstantBuffer // CBV
};

// Mirrors DXGI_FORMAT numerically for the subset of formats currently needed.
enum class Format
{
	Unknown = 0,
	R32G32B32A32_FLOAT = 2,
	R32G32_UINT = 4,
	R16G16B16A16_FLOAT = 10,
	R16G16_FLOAT = 34,
	R8G8B8A8_UNORM = 28,
	R8G8B8A8_UNORM_SRGB = 29,
	R10G10B10A2_UNORM = 24,
	R32_TYPELESS = 39,
	D32_FLOAT = 40,
	R32_FLOAT = 41,
	R32_UINT = 42,
	R24G8_TYPELESS = 44,
	D24_UNORM_S8_UINT = 45,
	R16_FLOAT = 54,
	R8_UNORM = 61,
	B8G8R8A8_UNORM = 87,
	BC7_UNORM = 98,
	BC7_UNORM_SRGB = 99
};

enum class TextureDimension
{
	Texture1D,
	Texture2D,
	Texture3D,
	TextureCube
};

inline constexpr uint32_t kCubeFaceCount = 6;

struct TextureViewDesc
{
	Format format = Format::Unknown; // Unknown = the resource's own format
	ResourceViewType viewType = ResourceViewType::ShaderResource; // SRV or UAV
	TextureDimension dimension = TextureDimension::Texture2D;
	bool asArray = false; // Texture1D/Texture2D only: view as an array instead of a single slice
	uint32_t baseMip = 0; // SRV: most detailed mip. UAV: the mip slice
	uint32_t mipCount = 0; // SRV only (a UAV always covers exactly one mip). 0 = all from baseMip
	uint32_t firstSlice = 0; // First array slice; first W slice for a 3D texture; first face (multiple of 6) for a cube SRV
	uint32_t sliceCount = 0; // Slices covered. 0 = all from firstSlice. Ignored for non-array views and cube SRVs
};

enum class ResourceUsageFlags : uint32_t
{
	None = 0,
	RenderTarget = 1 << 0,
	DepthStencil = 1 << 1,
	UnorderedAccess = 1 << 2,
};

inline ResourceUsageFlags operator|(ResourceUsageFlags a, ResourceUsageFlags b)
{
	return static_cast<ResourceUsageFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

struct TextureDesc
{
	TextureDimension dimension = TextureDimension::Texture2D;
	uint width = 1;
	uint height = 1;
	uint depthOrArraySize = 1;
	uint mipLevels = 1;
	Format format = Format::R8G8B8A8_UNORM;
	ResourceUsageFlags usageFlags = ResourceUsageFlags::None;
	ResourceLayout initialLayout = ResourceLayout::Undefined;
	std::string debugName;
};

struct BufferDesc
{
	uint64_t sizeInBytes = 0;
	uint32_t structureByteStride = 0; // 0 = raw/typed buffer, non-zero = structured buffer
	ResourceUsageFlags usageFlags = ResourceUsageFlags::None;
	ResourceLayout initialLayout = ResourceLayout::Undefined;
	std::string debugName;
};

static constexpr uint32_t InvalidResourceDescriptorIndex = UINT32_MAX;

struct ResourceViewInfo
{
	ResourceHandle handle = InvalidResourceHandle;
	DescriptorHandle descriptorIndex = InvalidDescriptorHandle; // raw bindless descriptor-heap slot index
	uint64_t cpuDescriptorHandle = 0;
	uint64_t gpuDescriptorHandle = 0;
};

enum class ResourceKind : uint8_t
{
	Unknown,
	Texture,
	Buffer
};

struct Resource
{
	void *ptr = nullptr;
	ResourceKind kind = ResourceKind::Unknown;
};

struct ResourceTransitionDesc
{
	ResourceHandle resource;
	uint subresource;
	ResourceLayout layoutBefore;
	ResourceLayout layoutAfter;
	ResourceSync syncBefore;
	ResourceSync syncAfter;
	ResourceAccess accessBefore;
	ResourceAccess accessAfter;
};

struct ClearValue
{
	float colour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float depth = 0.0f;
};

struct TextureMipData
{
	const void *pixels = nullptr;
	uint32_t rowPitch = 0;
	uint32_t rowCount = 0; // for a 3D texture, this is rowsPerDepthSlice; total rows = rowCount * depth
	uint32_t depth = 1; // depth slices at this mip (Texture3D only; ignored otherwise)
};

using UploadBatchHandle = uint32_t;
static constexpr UploadBatchHandle InvalidUploadBatchHandle = UINT32_MAX;

using ShaderTableHandle = int16;
static constexpr ShaderTableHandle InvalidShaderTableHandle = -1;

using AccelerationStructureHandle = uint32_t;
static constexpr AccelerationStructureHandle InvalidAccelerationStructureHandle = UINT32_MAX;

using CommandSignatureHandle = int16;
static constexpr CommandSignatureHandle InvalidCommandSignatureHandle = -1;

struct RaytracingHitGroup
{
	std::string hitGroupExport;
	std::string closestHitExport;
	std::string anyHitExport; // empty = none
	std::string intersectionExport; // empty = triangles
};

struct RaytracingPipelineDesc
{
	std::vector<std::string> exports; // raygen/miss/closesthit entry names to export from the lib
	std::vector<RaytracingHitGroup> hitGroups;
	uint32_t maxPayloadSizeInBytes = 0;
	uint32_t maxAttributeSizeInBytes = 8;
	uint32_t maxTraceRecursionDepth = 1;
	std::vector<std::string>
			defines; // Slang preprocessor defines ("NAME" or "NAME=VALUE"); use them for shader permutations
	std::string profile = "lib_6_6"; // DXIL library target, "lib_6_9" for HitObject / MaybeReorderThread
	bool allowOpacityMicromaps = false;
};

// DXR 1.2 shader execution reordering (HitObject and MaybeReorderThread in lib_6_9 shaders).
struct ShaderExecutionReorderingSupport
{
	bool supported = false;
	bool actuallyReorders = false; // false where MaybeReorderThread is a no-op
};

// GPU memory owned by the render context's allocator.
struct GPUMemoryUsage
{
	uint32_t blockCount = 0;      // heaps and committed resources created from the driver
	uint32_t allocationCount = 0; // resources placed in those blocks
	uint64_t blockBytes = 0;      // reserved from the driver
	uint64_t allocationBytes = 0; // of which in use by resources
};

// A memory segment group as the OS sees it.
struct GPUMemorySegment
{
	GPUMemoryUsage allocator;
	uint64_t usageBytes = 0;  // the whole process, including what the allocator does not own (swap chain, PSOs, ...)
	uint64_t budgetBytes = 0; // what the OS currently lets the process use before it starts paging
};

struct GPUMemoryStats
{
	bool unifiedMemory = false; // integrated GPU: everything lives in the local segment
	GPUMemorySegment local;     // video memory
	GPUMemorySegment nonLocal;  // system memory the GPU reaches over the bus
};

struct BLASGeometry
{
	ResourceHandle vertexBuffer = InvalidResourceHandle;
	uint64_t vertexBufferOffset = 0; // bytes; lets several BLASes share one vertex buffer
	uint32_t vertexCount = 0;
	uint32_t vertexStride = 0; // typically sizeof(InterleavedVertex); only the leading float3 position is read
	ResourceHandle indexBuffer = InvalidResourceHandle;
	uint64_t indexBufferOffset = 0; // bytes, must be a multiple of 4 (R32_UINT indices)
	uint32_t indexCount = 0;
	bool isOpaque = true;
	bool noDuplicateAnyHit = false; // one any-hit call per primitive per ray, for accumulating any-hit shaders
	// DXR 1.2 opacity micromaps (SupportsOpacityMicromaps): an int32 per triangle, an index into the array or
	// a special index (OpacityMicromapSpecialIndex). The array must outlive the BLAS. Invalid for none.
	AccelerationStructureHandle opacityMicromapArray = InvalidAccelerationStructureHandle;
	ResourceHandle opacityMicromapIndexBuffer = InvalidResourceHandle;
	uint64_t opacityMicromapIndexBufferOffset = 0; // bytes, a multiple of 4
};

// One opacity micromap of an array: 4^subdivisionLevel micro-triangles along the OC1 space-filling curve,
// with 1 bit (2 states) or 2 bits (4 states) each, starting byteOffset bytes into the array's data.
struct OpacityMicromap
{
	uint32_t byteOffset = 0;
	uint16_t subdivisionLevel = 0; // 0..12
	uint16_t states = 4; // 2 (transparent, opaque) or 4 (and unknown-transparent, unknown-opaque)
};

// Per-triangle micromap indices that stand for a whole triangle in one state, with no micromap.
enum OpacityMicromapSpecialIndex : int32_t
{
	OpacityMicromapFullyTransparent = -1,
	OpacityMicromapFullyOpaque = -2,
	OpacityMicromapFullyUnknownTransparent = -3,
	OpacityMicromapFullyUnknownOpaque = -4,
};

struct TLASInstance
{
	AccelerationStructureHandle blas = InvalidAccelerationStructureHandle;
	float transform[3][4] = {}; // row-major 3x4
	uint32_t instanceID = 0;
	uint32_t instanceMask = 0xFF;
	uint32_t hitGroupIndex = 0;
};

enum class IndirectCommandType
{
	Dispatch,
	Draw,
	DispatchMesh
};

class ITextureCompressor
{
public:
	virtual ~ITextureCompressor() = default;

	virtual bool Initialise() = 0;

	virtual bool IsReady() const = 0;

	virtual HRESULT Compress(const DirectX::ScratchImage &srcChain,
							 DXGI_FORMAT bcFormat,
							 DirectX::ScratchImage &result) = 0;
};

class RenderContext
{
public:
	RenderContext() = default;
	virtual ~RenderContext() = default;
	virtual void CreatePerFrameResources() = 0;
	virtual void BeginFrame() = 0;
	virtual void EndFrame() = 0;
	virtual void BeginGraphicsCommandList() = 0;
	virtual void EndGraphicsCommandList() = 0;
	virtual void WaitForGPUIdle() = 0;
	virtual void Present() = 0;

	// Sets up the ImGui backend for this API; call once after CreatePerFrameResources.
	virtual void InitialiseImgui(void *hwnd) = 0;
	virtual void ImguiNewFrame() = 0;
	// Records ImGui's draw data into the current graphics command list; call before ending it.
	virtual void ImguiRender() = 0;

	virtual void SetGraphicsPipeline(PipelineHandle pipelineHandle) = 0;
	virtual void SetDescriptorHeaps() = 0;

	// GPU debug annotation (shows up as nested events in RenderDoc/PIX). Always pair Begin with End.
	virtual void BeginEvent(const std::string &label) = 0;
	virtual void EndEvent() = 0;

	// GPU timestamp profiling on the graphics command list. Results arrive framesInFlight frames later, without
	// stalling, and each scope keeps a rolling window of samples. Scopes may nest; always pair Begin with End.
	virtual void BeginProfileScope(const std::string &name) = 0;
	virtual void EndProfileScope() = 0;
	// Null until the scope has at least one sample.
	virtual const GPUProfileScopeStats *GetProfileScopeStats(const std::string &name) const = 0;
	virtual std::vector<GPUProfileScopeStats> GetAllProfileScopeStats() const = 0;
	virtual void ResetProfiler() = 0;
	virtual void SetSRV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetUAV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetSRVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetUAVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetConstantBufferByName(const std::string &name, ResourceHandle resource) = 0;

	virtual void Draw(uint vertexCount = 3) = 0;

	virtual void DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;

	// defines are Slang preprocessor defines ("NAME" or "NAME=VALUE"); use them for shader permutations.
	virtual PipelineHandle CreateComputePipeline(const std::string &shaderPath,
												 const std::string &entryPoint,
												 const std::vector<std::string> &defines = {}) = 0;
	virtual void SetComputePipeline(PipelineHandle pipelineHandle) = 0;
	virtual void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;
	virtual void SetCompute32BitConstants(const void *data, uint32_t num32BitValues) = 0;
	virtual void SetComputeSRV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetComputeUAV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetComputeConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetComputeSRVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetComputeUAVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetComputeConstantBufferByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetComputeScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes) = 0;

	virtual PipelineHandle CreateRaytracingPipeline(const std::string &shaderPath,
													const RaytracingPipelineDesc &pipelineDesc) = 0;
	virtual void SetRaytracingPipeline(PipelineHandle pipelineHandle) = 0;
	// Root constants go wherever the raytracing shader's g_PushConstants/pushConstants CBV is declared.
	virtual void SetRaytracing32BitConstants(const void *data, uint32_t num32BitValues) = 0;
	virtual void SetRaytracingSRV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetRaytracingUAV(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetRaytracingConstantBuffer(uint32_t shaderRegister, ResourceHandle resource) = 0;
	virtual void SetRaytracingSRVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetRaytracingUAVByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetRaytracingConstantBufferByName(const std::string &name, ResourceHandle resource) = 0;
	virtual void SetRaytracingScalarByName(const std::string &name, const void *data, uint32_t sizeInBytes) = 0;

	virtual ShaderExecutionReorderingSupport GetShaderExecutionReorderingSupport() const = 0;
	// DXR 1.2 opacity micromaps, for TraceRay with RaytracingPipelineDesc::allowOpacityMicromaps and for lib_6_9
	// ray queries with RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS.
	virtual bool SupportsOpacityMicromaps() const = 0;

	// Cheap: running totals and the OS budget, which refreshes once per frame.
	virtual GPUMemoryStats GetMemoryStats() const = 0;

	virtual ShaderTableHandle CreateShaderTable(PipelineHandle raytracingPipeline) = 0;
	virtual void AddRayGenShaderRecord(ShaderTableHandle table, const std::string &exportName) = 0;
	virtual void AddMissShaderRecord(ShaderTableHandle table, const std::string &exportName) = 0;
	virtual void AddHitGroupShaderRecord(ShaderTableHandle table, const std::string &exportName) = 0;
	virtual void BuildShaderTable(ShaderTableHandle table) = 0;
	virtual void DispatchRays(ShaderTableHandle table, uint32_t width, uint32_t height, uint32_t depth) = 0;

	// Builds and compacts the array on the compute queue before it returns, so BLASes can link it.
	virtual AccelerationStructureHandle CreateOpacityMicromapArray(std::span<const uint8_t> data,
																   std::span<const OpacityMicromap> micromaps) = 0;
	virtual AccelerationStructureHandle CreateBLAS(const BLASGeometry &geometry) = 0;
	virtual AccelerationStructureHandle CreateTLAS(std::span<const TLASInstance> instances) = 0;
	virtual void BuildAccelerationStructure(AccelerationStructureHandle handle) = 0;
	// Replaces a TLAS's instances, as many as it was created with, and rebuilds it on the current
	// frame's graphics command list. Follow with UAVBarrierForAccelerationStructures before tracing it.
	virtual void UpdateTLAS(AccelerationStructureHandle handle, std::span<const TLASInstance> instances) = 0;
	virtual void BuildCompactedBLASes(std::span<const AccelerationStructureHandle> handles) = 0;
	virtual ResourceHandle GetAccelerationStructureSRV(AccelerationStructureHandle handle) = 0;
	virtual void DestroyAccelerationStructure(AccelerationStructureHandle handle) = 0;
	virtual void UAVBarrierForAccelerationStructures() = 0;

	// pipeline supplies the root signature for root-constants > 0; ignored otherwise (pass InvalidPipelineHandle).
	virtual CommandSignatureHandle CreateCommandSignature(IndirectCommandType type,
														  uint32_t rootConstantsSizeInBytes,
														  PipelineHandle pipeline) = 0;
	virtual void ExecuteIndirect(CommandSignatureHandle signature,
								 ResourceHandle argumentBuffer,
								 uint64_t argumentBufferOffset,
								 uint32_t maxCommandCount,
								 ResourceHandle countBuffer = InvalidResourceHandle,
								 uint64_t countBufferOffset = 0) = 0;

	virtual void SetPrimitiveTopology(PrimitiveTopology topology = PrimitiveTopology::TriangleList) = 0;
	virtual void ClearRenderTarget(Resource renderTarget, float clearColour[4]) = 0;
	virtual void ClearDepthStencil(Resource depthStencil, float depth = 1.0f, uint8_t stencil = 0) = 0;
	virtual void ClearUAVBufferUint(ResourceHandle handle, uint32_t clearValue = 0) = 0;
	virtual void SetRenderTargets(const Resource *renderTargets,
								  uint numRenderTargets,
								  Resource depthStencil = {},
								  bool readOnly = false) = 0;
	virtual void SetViewport(const Viewport &viewport) = 0;
	virtual void SetScissorRect(const ScissorRect &scissorRect) = 0;

	virtual Resource GetCurrentBackBuffer() = 0;
	virtual ResourceHandle GetCurrentBackBufferHandle() const = 0;
	virtual Format GetBackBufferFormat() const = 0;

	virtual PipelineHandle CreateGraphicsPipeline(const std::string &shaderPath,
												  std::span<PipelineEntryPoint> entryPoints,
												  const GraphicsPipelineCreateInfo &pipelineInfo) = 0;

	virtual ResourceHandle CreateTexture(const TextureDesc &desc, ResourceViewType viewType) = 0;
	virtual ResourceHandle CreateBuffer(const BufferDesc &desc, ResourceViewType viewType) = 0;

	// Additional bindless view (non-owning) over an existing texture, e.g. UAV bake -> SRV sample, or a
	// UAV over one mip of a cube map (dimension = TextureCube: all six faces as a 2D array).
	// CAUTION: the handles track resource-state independently; only transition the owning handle,
	// and destroy views before the owner.
	virtual ResourceHandle CreateTextureView(ResourceHandle existingTexture, const TextureViewDesc &desc) = 0;

	virtual std::vector<ResourceHandle> CreateBufferArray(std::span<const BufferDesc> descs,
														  ResourceViewType viewType) = 0;

	virtual void SetComputeUAVTableByName(const std::string &name, ResourceHandle firstInArray) = 0;
	virtual void SetComputeSRVTableByName(const std::string &name, ResourceHandle firstInArray) = 0;
	virtual void SetUAVTableByName(const std::string &name, ResourceHandle firstInArray) = 0;

	virtual UploadBatchHandle BeginUpload() = 0;
	virtual void EndUpload(UploadBatchHandle batch) = 0;

	virtual ResourceHandle CreateBuffer(const BufferDesc &desc,
										ResourceViewType viewType,
										const void *initialData,
										UploadBatchHandle batch) = 0;
	virtual ResourceHandle CreateTexture(const TextureDesc &desc,
										 ResourceViewType viewType,
										 std::span<const TextureMipData> mipData,
										 UploadBatchHandle batch) = 0;

	virtual ResourceHandle CreateConstantBuffer(uint64_t sizeInBytes, const std::string &debugName = {}) = 0;
	virtual void UpdateConstantBuffer(ResourceHandle handle, const void *data, uint64_t sizeInBytes) = 0;

	virtual ResourceHandle CreateUploadBuffer(const BufferDesc &desc,
											  ResourceViewType viewType,
											  const void *initialData = nullptr) = 0;

	virtual void UpdateUploadBuffer(ResourceHandle handle, const void *data, uint64_t offset, uint64_t sizeInBytes) = 0;

	virtual void CopyTexture(ResourceHandle dstHandle, ResourceHandle srcHandle) = 0;

	virtual void CopyBufferRegion(ResourceHandle dstHandle,
								  uint64_t dstOffset,
								  ResourceHandle srcHandle,
								  uint64_t srcOffset,
								  uint64_t sizeInBytes) = 0;

	virtual void SetGraphics32BitConstants(const void *data, uint32_t num32BitValues) = 0;
	virtual ResourceViewInfo GetResourceViewInfo(ResourceHandle handle) const = 0;
	virtual Resource GetResource(ResourceHandle handle) const = 0;
	virtual void TransitionResource(ResourceHandle handle,
									ResourceLayout newLayout,
									ResourceSync newSync,
									ResourceAccess newAccess) = 0;

	// Barrier covering every resource at once, for ordering back-to-back dispatches that share a
	// large set of UAVs. Cheaper than transitioning each buffer individually when a whole pass
	// group has to be ordered as a unit.
	virtual void GlobalBarrier(ResourceSync syncBefore,
							   ResourceAccess accessBefore,
							   ResourceSync syncAfter,
							   ResourceAccess accessAfter) = 0;

	virtual void DestroyResource(ResourceHandle handle) = 0;

	virtual std::unique_ptr<ITextureCompressor> CreateTextureCompressor() = 0;

	uint GetCurrentFrameIndex() const
	{
		return currentFrameIndex_;
	}

	uint GetFramesInFlight() const
	{
		return framesInFlight_;
	}

	uint GetSwapchainImageCount() const
	{
		return swapchainImageCount_;
	}

	uint GetCurrentFrame() const
	{
		return currentFrame_;
	}

	// Output/swapchain resolution - fixed for the life of the window.
	uint GetWindowWidth() const
	{
		return windowWidth_;
	}

	uint GetWindowHeight() const
	{
		return windowHeight_;
	}

	uint GetRenderWidth() const
	{
		return renderWidth_ == 0 ? windowWidth_ : renderWidth_;
	}

	uint GetRenderHeight() const
	{
		return renderHeight_ == 0 ? windowHeight_ : renderHeight_;
	}

	void SetRenderResolution(uint width, uint height)
	{
		renderWidth_ = width;
		renderHeight_ = height;
	}

	bool GetUseHDR() const
	{
		return useHDR_;
	}

protected:
	uint currentFrame_ = 0;
	uint currentFrameIndex_ = 0;
	uint framesInFlight_ = 0;
	uint swapchainImageCount_ = 0;
	uint windowWidth_ = 0;
	uint windowHeight_ = 0;
	uint renderWidth_ = 0;
	uint renderHeight_ = 0;
	bool useHDR_ = false;
};

RenderContext *CreateRenderContext(Window *window,
								   RenderAPI api,
								   uint framesInFlight,
								   uint swapchainImageCount,
								   bool useSoftwareAdapter = false,
								   bool useHDR = false);
