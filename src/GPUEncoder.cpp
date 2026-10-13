#include "GPUEncoder.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cassert>
#include <vector>

#undef min
#undef max

using Microsoft::WRL::ComPtr;

namespace
{
	// Shader constant values that mirror the #defines in the HLSL.
	constexpr uint32_t kFormatBC7_UNORM = 98;
	constexpr uint32_t kFormatBC6H_UF16 = 95;
	constexpr uint32_t kFormatBC6H_SF16 = 96;

	// Cap on the number of blocks submitted per Dispatch so a single call does
	// not run long enough to trip the GPU watchdog (TDR).
	constexpr uint32_t kMaxBlocksPerDispatch = 16384;

	constexpr uint32_t Align(uint32_t value, uint32_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	bool IsBC6H(DXGI_FORMAT fmt)
	{
		return fmt == DXGI_FORMAT_BC6H_UF16 || fmt == DXGI_FORMAT_BC6H_SF16;
	}

	bool IsBC7(DXGI_FORMAT fmt)
	{
		return fmt == DXGI_FORMAT_BC7_UNORM || fmt == DXGI_FORMAT_BC7_UNORM_SRGB;
	}

	// The format value passed to the shader constant buffer.
	uint32_t ShaderFormatValue(DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
			case DXGI_FORMAT_BC6H_UF16:
				return kFormatBC6H_UF16;
			case DXGI_FORMAT_BC6H_SF16:
				return kFormatBC6H_SF16;
			default:
				return kFormatBC7_UNORM;
		}
	}

	// The uncompressed input texture format the shaders expect to Load() from.
	DXGI_FORMAT SourceFormatFor(DXGI_FORMAT bcFormat)
	{
		if (IsBC6H(bcFormat))
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
} // namespace

GPUEncoder::GPUEncoder(D3D12Device *device, D3D12CommandQueue *computeQueue) :
	device_(device), computeQueue_(computeQueue)
{
}

bool GPUEncoder::Initialise()
{
	if (!device_ || !computeQueue_)
		return false;

	if (!BuildRootSignature())
		return false;

	const wchar_t *kBC7Path = L"" GPU_ENCODER_SHADER_DIR "BC7Encode.hlsl";
	const wchar_t *kBC6HPath = L"" GPU_ENCODER_SHADER_DIR "BC6HEncode.hlsl";

	struct PassDesc
	{
		const wchar_t *path;
		const char *entry;
		ComPtr<ID3D12PipelineState> *target;
	};

	const PassDesc passes[] = {
		{ kBC7Path, "TryMode456CS", &bc7TryMode456_ },	 { kBC7Path, "TryMode137CS", &bc7TryMode137_ },
		{ kBC7Path, "TryMode02CS", &bc7TryMode02_ },	 { kBC7Path, "EncodeBlockCS", &bc7Encode_ },
		{ kBC6HPath, "TryModeG10CS", &bc6hTryModeG10_ }, { kBC6HPath, "TryModeLE10CS", &bc6hTryModeLE10_ },
		{ kBC6HPath, "EncodeBlockCS", &bc6hEncode_ },
	};

	for (const PassDesc &pass: passes)
	{
		ComPtr<ID3DBlob> blob;
		if (!CompileShader(pass.path, pass.entry, blob))
			return false;
		if (!BuildPSO(blob.Get(), *pass.target))
			return false;
	}

	// Persistent compute command list and fence, reused across every mip. The
	// list is created ready to record; we Reset() it at the top of each
	// CompressMip and Close() before submission.
	cmdList_ = device_->CreateCommandList(D3D12CommandListType::Compute);
	if (!cmdList_)
		return false;
	cmdList_->SetName("GPUEncoder_CompressList");

	fence_ = device_->CreateFence(0);
	if (!fence_)
		return false;
	fenceValue_ = 0;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = kDescriptorCount;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ID3D12Device *dev = device_->GetNativeDevice().Get();
	if (FAILED(dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_))))
		return false;
	descriptorSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	ready_ = true;
	return true;
}

bool GPUEncoder::CompileShader(const wchar_t *path, const char *entryPoint, ComPtr<ID3DBlob> &outBlob)
{
	// Compile with the legacy FXC compiler (D3DCompile). These shaders only use
	// SM5-era features, so cs_5_1 is sufficient and is the highest model FXC
	// supports. REF_DEVICE makes the shaders insert the group sync barriers they
	// need to be deterministic across vendors.
	const D3D_SHADER_MACRO macros[] = {
		{ "REF_DEVICE", "1" },
		{ nullptr, nullptr },
	};

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3DCompileFromFile(path,
									macros,
									D3D_COMPILE_STANDARD_FILE_INCLUDE,
									entryPoint,
									"cs_5_1",
									flags,
									0,
									&outBlob,
									&errors);
	if (FAILED(hr))
	{
		if (errors)
			OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
		return false;
	}
	return true;
}

bool GPUEncoder::BuildRootSignature()
{
	// [0] CBV  b0  EncoderCB
	// [1] SRV  t0  g_Input  (descriptor table, lives in the shared heap)
	// [2] SRV  t1  g_InBuff (descriptor table)
	// [3] UAV  u0  g_OutBuff (descriptor table)
	D3D12_DESCRIPTOR_RANGE rangeInputTex{};
	rangeInputTex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	rangeInputTex.NumDescriptors = 1;
	rangeInputTex.BaseShaderRegister = 0; // t0
	rangeInputTex.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE rangeInputBuf{};
	rangeInputBuf.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	rangeInputBuf.NumDescriptors = 1;
	rangeInputBuf.BaseShaderRegister = 1; // t1
	rangeInputBuf.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE rangeOutput{};
	rangeOutput.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	rangeOutput.NumDescriptors = 1;
	rangeOutput.BaseShaderRegister = 0; // u0
	rangeOutput.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER params[4]{};

	params[kRootCBV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[kRootCBV].Descriptor.ShaderRegister = 0; // b0
	params[kRootCBV].Descriptor.RegisterSpace = 0;
	params[kRootCBV].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[kRootSRVTex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[kRootSRVTex].DescriptorTable.NumDescriptorRanges = 1;
	params[kRootSRVTex].DescriptorTable.pDescriptorRanges = &rangeInputTex;
	params[kRootSRVTex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[kRootSRVBuf].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[kRootSRVBuf].DescriptorTable.NumDescriptorRanges = 1;
	params[kRootSRVBuf].DescriptorTable.pDescriptorRanges = &rangeInputBuf;
	params[kRootSRVBuf].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[kRootUAV].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[kRootUAV].DescriptorTable.NumDescriptorRanges = 1;
	params[kRootUAV].DescriptorTable.pDescriptorRanges = &rangeOutput;
	params[kRootUAV].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = _countof(params);
	desc.pParameters = params;
	desc.NumStaticSamplers = 0;
	desc.pStaticSamplers = nullptr;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> serialised;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialised, &error);
	if (FAILED(hr))
		return false;

	hr = device_->GetNativeDevice()->CreateRootSignature(0,
														 serialised->GetBufferPointer(),
														 serialised->GetBufferSize(),
														 IID_PPV_ARGS(&rootSignature_));
	return SUCCEEDED(hr);
}

bool GPUEncoder::BuildPSO(ID3DBlob *csBlob, ComPtr<ID3D12PipelineState> &outPSO)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = rootSignature_.Get();
	desc.CS.pShaderBytecode = csBlob->GetBufferPointer();
	desc.CS.BytecodeLength = csBlob->GetBufferSize();

	HRESULT hr = device_->GetNativeDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPSO));
	return SUCCEEDED(hr);
}

HRESULT GPUEncoder::Compress(const DirectX::ScratchImage &srcChain, DXGI_FORMAT bcFormat, DirectX::ScratchImage &result)
{
	if (!ready_)
		return E_NOT_VALID_STATE;

	if (!IsBC6H(bcFormat) && !IsBC7(bcFormat))
		return E_INVALIDARG; // GPU path only supports BC6H / BC7

	const DirectX::TexMetadata &meta = srcChain.GetMetadata();

	DirectX::TexMetadata dstMeta = meta;
	dstMeta.format = bcFormat;

	HRESULT hr = result.Initialize(dstMeta);
	if (FAILED(hr))
		return hr;

	const float alphaWeight = IsBC7(bcFormat) ? 1.0f : 0.0f;

	for (size_t item = 0; item < meta.arraySize; ++item)
	{
		for (size_t mip = 0; mip < meta.mipLevels; ++mip)
		{
			const DirectX::Image *srcMip = srcChain.GetImage(mip, item, 0);
			const DirectX::Image *dstMip = result.GetImage(mip, item, 0);
			if (!srcMip || !dstMip)
				return E_FAIL;

			hr = CompressMip(*srcMip, bcFormat, alphaWeight, dstMip->pixels, dstMip->rowPitch);
			if (FAILED(hr))
				return hr;
		}
	}

	return S_OK;
}

HRESULT GPUEncoder::CompressMip(
		const DirectX::Image &srcMip, DXGI_FORMAT bcFormat, float alphaWeight, uint8_t *dstPixels, size_t alignedPitch)
{
	ID3D12Device *dev = device_->GetNativeDevice().Get();

	const uint32_t width = static_cast<uint32_t>(srcMip.width);
	const uint32_t height = static_cast<uint32_t>(srcMip.height);
	const uint32_t numBlockX = std::max(1u, (width + 3) / 4);
	const uint32_t numBlockY = std::max(1u, (height + 3) / 4);
	const uint32_t numTotalBlocks = numBlockX * numBlockY;

	const DXGI_FORMAT srcFormat = SourceFormatFor(bcFormat);


	D3D12_HEAP_PROPERTIES defaultHeap{};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_HEAP_PROPERTIES uploadHeap{};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_HEAP_PROPERTIES readbackHeap{};
	readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

	// The source is padded out to whole blocks by repeating its last column and row, as the
	// shaders read every texel of a block and would otherwise fit the padding as black.
	D3D12_RESOURCE_DESC texDesc{};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = numBlockX * 4;
	texDesc.Height = numBlockY * 4;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = srcFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ComPtr<ID3D12Resource> srcTexture;
	HRESULT hr = dev->CreateCommittedResource(&defaultHeap,
											  D3D12_HEAP_FLAG_NONE,
											  &texDesc,
											  D3D12_RESOURCE_STATE_COPY_DEST,
											  nullptr,
											  IID_PPV_ARGS(&srcTexture));
	if (FAILED(hr))
		return hr;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	uint32_t rowCount = 0;
	uint64_t rowSizeInBytes = 0;
	uint64_t uploadSize = 0;
	dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);

	D3D12_RESOURCE_DESC uploadDesc{};
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = uploadSize;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> uploadBuffer;
	hr = dev->CreateCommittedResource(&uploadHeap,
									  D3D12_HEAP_FLAG_NONE,
									  &uploadDesc,
									  D3D12_RESOURCE_STATE_GENERIC_READ,
									  nullptr,
									  IID_PPV_ARGS(&uploadBuffer));
	if (FAILED(hr))
		return hr;

	// Copy the source image row by row, honouring the placement row pitch which
	// is aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, and padding it to whole blocks.
	{
		uint8_t *mapped = nullptr;
		D3D12_RANGE noRead{ 0, 0 };
		hr = uploadBuffer->Map(0, &noRead, reinterpret_cast<void **>(&mapped));
		if (FAILED(hr))
			return hr;

		const size_t texelBytes = static_cast<size_t>(rowSizeInBytes) / texDesc.Width;
		const size_t srcRowBytes = texelBytes * width;
		for (uint32_t y = 0; y < rowCount; ++y)
		{
			uint8_t *dstRow = mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
			const uint8_t *srcRow = srcMip.pixels + static_cast<size_t>(std::min(y, height - 1)) * srcMip.rowPitch;
			memcpy(dstRow, srcRow, srcRowBytes);
			for (size_t x = srcRowBytes; x < rowSizeInBytes; x += texelBytes)
				memcpy(dstRow + x, srcRow + srcRowBytes - texelBytes, texelBytes);
		}
		uploadBuffer->Unmap(0, nullptr);
	}

	// 2. Two ping-pong StructuredBuffers (uint4 per block) for the Try passes,
	//    plus a readback buffer for the final compressed output.
	const uint64_t blockBufferSize = static_cast<uint64_t>(numTotalBlocks) * sizeof(uint32_t) * 4;

	auto createUavBuffer = [&](ComPtr<ID3D12Resource> &out) -> HRESULT
	{
		D3D12_RESOURCE_DESC bufDesc{};
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Width = blockBufferSize;
		bufDesc.Height = 1;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.MipLevels = 1;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		// Buffers are always created in COMMON and promoted on first use; passing
		// UNORDERED_ACCESS here only produces a STATE_IGNORED warning.
		return dev->CreateCommittedResource(&defaultHeap,
											D3D12_HEAP_FLAG_NONE,
											&bufDesc,
											D3D12_RESOURCE_STATE_COMMON,
											nullptr,
											IID_PPV_ARGS(&out));
	};

	ComPtr<ID3D12Resource> bufferA;
	ComPtr<ID3D12Resource> bufferB;
	if (FAILED(hr = createUavBuffer(bufferA)))
		return hr;
	if (FAILED(hr = createUavBuffer(bufferB)))
		return hr;

	D3D12_RESOURCE_DESC readbackDesc{};
	readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	readbackDesc.Width = blockBufferSize;
	readbackDesc.Height = 1;
	readbackDesc.DepthOrArraySize = 1;
	readbackDesc.MipLevels = 1;
	readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
	readbackDesc.SampleDesc.Count = 1;
	readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> readbackBuffer;
	hr = dev->CreateCommittedResource(&readbackHeap,
									  D3D12_HEAP_FLAG_NONE,
									  &readbackDesc,
									  D3D12_RESOURCE_STATE_COPY_DEST,
									  nullptr,
									  IID_PPV_ARGS(&readbackBuffer));
	if (FAILED(hr))
		return hr;

	// 3. Constant buffer (one EncoderCB per Dispatch). We pack a slot per
	//    chunk so the values stay alive until the GPU has consumed them.
	const uint32_t numChunks = (numTotalBlocks + kMaxBlocksPerDispatch - 1) / kMaxBlocksPerDispatch;
	// Each chunk issues one Dispatch per pass and writes one CB slot per
	// Dispatch. Count the passes exactly:
	//   BC7:  456 + (137 x3) + (02 x2) + encode = 7
	//   BC6H: G10 + (LE10 x4) + encode          = 6
	const uint32_t cbSlotSize =
			Align(static_cast<uint32_t>(sizeof(EncoderCB)), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	const uint32_t passesPerChunk = IsBC7(bcFormat) ? 7u : 6u;
	const uint32_t cbCount = numChunks * passesPerChunk;

	D3D12_RESOURCE_DESC cbDesc{};
	cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	cbDesc.Width = static_cast<uint64_t>(cbSlotSize) * cbCount;
	cbDesc.Height = 1;
	cbDesc.DepthOrArraySize = 1;
	cbDesc.MipLevels = 1;
	cbDesc.Format = DXGI_FORMAT_UNKNOWN;
	cbDesc.SampleDesc.Count = 1;
	cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> cbResource;
	hr = dev->CreateCommittedResource(&uploadHeap,
									  D3D12_HEAP_FLAG_NONE,
									  &cbDesc,
									  D3D12_RESOURCE_STATE_GENERIC_READ,
									  nullptr,
									  IID_PPV_ARGS(&cbResource));
	if (FAILED(hr))
		return hr;

	uint8_t *cbMapped = nullptr;
	{
		D3D12_RANGE noRead{ 0, 0 };
		if (FAILED(hr = cbResource->Map(0, &noRead, reinterpret_cast<void **>(&cbMapped))))
			return hr;
	}
	uint32_t cbCursor = 0;
	const D3D12_GPU_VIRTUAL_ADDRESS cbBaseGpu = cbResource->GetGPUVirtualAddress();

	auto writeCB = [&](uint32_t startBlock, uint32_t modeId) -> D3D12_GPU_VIRTUAL_ADDRESS
	{
		assert(cbCursor < cbCount);
		EncoderCB cb{};
		cb.texWidth = numBlockX * 4;
		cb.numBlockX = numBlockX;
		cb.format = ShaderFormatValue(bcFormat);
		cb.modeId = modeId;
		cb.startBlockId = startBlock;
		cb.numTotalBlocks = numTotalBlocks;
		cb.alphaWeight = alphaWeight;
		const uint32_t offset = cbCursor * cbSlotSize;
		memcpy(cbMapped + offset, &cb, sizeof(cb));
		++cbCursor;
		return cbBaseGpu + offset;
	};

	// 4. Descriptors. We need an SRV for the source texture, plus an SRV and a
	//    UAV for each of the two ping-pong buffers.
	enum
	{
		kDescTex = 0,
		kDescASRV = 1,
		kDescAUAV = 2,
		kDescBSRV = 3,
		kDescBUAV = 4,
	};
	static_assert(kDescBUAV + 1 == kDescriptorCount);

	// The previous mip's work has finished (CompressMip waits on it), so its views can be overwritten.
	auto cpuHandle = [&](uint32_t slot) -> D3D12_CPU_DESCRIPTOR_HANDLE
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize_;
		return handle;
	};
	auto gpuHandle = [&](uint32_t slot) -> D3D12_GPU_DESCRIPTOR_HANDLE
	{
		D3D12_GPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<UINT64>(slot) * descriptorSize_;
		return handle;
	};

	// Source texture SRV.
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = srcFormat;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		dev->CreateShaderResourceView(srcTexture.Get(), &srv, cpuHandle(kDescTex));
	}

	auto createBufferViews = [&](ID3D12Resource *res, uint32_t srvSlot, uint32_t uavSlot)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_UNKNOWN;
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = numTotalBlocks;
		srv.Buffer.StructureByteStride = sizeof(uint32_t) * 4;
		dev->CreateShaderResourceView(res, &srv, cpuHandle(srvSlot));

		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav.Buffer.NumElements = numTotalBlocks;
		uav.Buffer.StructureByteStride = sizeof(uint32_t) * 4;
		dev->CreateUnorderedAccessView(res, nullptr, &uav, cpuHandle(uavSlot));
	};

	createBufferViews(bufferA.Get(), kDescASRV, kDescAUAV);
	createBufferViews(bufferB.Get(), kDescBSRV, kDescBUAV);

	// 5. Record the command list. The persistent list is reset for this mip.
	cmdList_->Reset();
	ID3D12GraphicsCommandList *cl = cmdList_->GetNativeCommandList().Get();

	ID3D12DescriptorHeap *heaps[] = { descriptorHeap_.Get() };
	cl->SetDescriptorHeaps(1, heaps);
	cl->SetComputeRootSignature(rootSignature_.Get());

	// Upload the source texture and transition it to a shader resource.
	{
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = srcTexture.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = uploadBuffer.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = footprint;

		cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		D3D12_RESOURCE_BARRIER toSrv{};
		toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toSrv.Transition.pResource = srcTexture.Get();
		toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &toSrv);
	}

	cl->SetComputeRootDescriptorTable(kRootSRVTex, gpuHandle(kDescTex));

	auto uavBarrier = [&](ID3D12Resource *res)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		b.UAV.pResource = res;
		cl->ResourceBarrier(1, &b);
	};

	auto transition = [](ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.StateBefore = before;
		b.Transition.StateAfter = after;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		return b;
	};

	// Tracks which buffer currently holds the "best so far" results. Each Try
	// pass reads from `in` and writes to `out`, then we swap.
	ID3D12Resource *inRes = bufferA.Get();
	ID3D12Resource *outRes = bufferB.Get();
	uint32_t inSRV = kDescASRV;
	uint32_t outUAV = kDescBUAV;

	// A buffer implicitly promoted to a read state cannot then be written as a
	// UAV, so the ping-pong pair is transitioned explicitly.
	{
		const D3D12_RESOURCE_BARRIER initial[] = {
			transition(inRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			transition(outRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cl->ResourceBarrier(_countof(initial), initial);
	}

	auto swapBuffers = [&]()
	{
		const D3D12_RESOURCE_BARRIER swap[] = {
			transition(outRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			transition(inRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		cl->ResourceBarrier(_countof(swap), swap);
		std::swap(inRes, outRes);
		if (inSRV == kDescASRV)
		{
			inSRV = kDescBSRV;
			outUAV = kDescAUAV;
		}
		else
		{
			inSRV = kDescASRV;
			outUAV = kDescBUAV;
		}
	};

	// Bind the current in (t1) / out (u0) tables and run one pass over all
	// chunks for a given mode_id.
	auto runPass = [&](ID3D12PipelineState *pso, uint32_t modeId, uint32_t threadsPerBlock)
	{
		cl->SetPipelineState(pso);
		cl->SetComputeRootDescriptorTable(kRootSRVBuf, gpuHandle(inSRV));
		cl->SetComputeRootDescriptorTable(kRootUAV, gpuHandle(outUAV));

		for (uint32_t start = 0; start < numTotalBlocks; start += kMaxBlocksPerDispatch)
		{
			const uint32_t count = std::min(kMaxBlocksPerDispatch, numTotalBlocks - start);
			cl->SetComputeRootConstantBufferView(kRootCBV, writeCB(start, modeId));

			// 16-thread passes pack 4 blocks per group; 64-thread passes pack 1.
			const uint32_t blocksPerGroup = (threadsPerBlock == 16) ? 4u : 1u;
			const uint32_t groups = (count + blocksPerGroup - 1) / blocksPerGroup;
			cl->Dispatch(groups, 1, 1);
		}
	};

	if (IsBC7(bcFormat))
	{
		// mode 4/5/6: one subset, 16 threads per block.
		runPass(bc7TryMode456_.Get(), 0, 16);
		swapBuffers();

		// modes 1, 3, 7: two subsets, 64 threads per block (one per partition).
		const uint32_t modes137[] = { 1, 3, 7 };
		for (uint32_t m: modes137)
		{
			runPass(bc7TryMode137_.Get(), m, 64);
			swapBuffers();
		}

		// modes 0, 2: three subsets, 64 threads per block.
		const uint32_t modes02[] = { 0, 2 };
		for (uint32_t m: modes02)
		{
			runPass(bc7TryMode02_.Get(), m, 64);
			swapBuffers();
		}

		// Final encode reads the winning mode/partition and writes the block.
		cl->SetPipelineState(bc7Encode_.Get());
		cl->SetComputeRootDescriptorTable(kRootSRVBuf, gpuHandle(inSRV));
		cl->SetComputeRootDescriptorTable(kRootUAV, gpuHandle(outUAV));
		for (uint32_t start = 0; start < numTotalBlocks; start += kMaxBlocksPerDispatch)
		{
			const uint32_t count = std::min(kMaxBlocksPerDispatch, numTotalBlocks - start);
			cl->SetComputeRootConstantBufferView(kRootCBV, writeCB(start, 0));
			const uint32_t groups = (count + 3) / 4; // 16 threads/block, 4 per group
			cl->Dispatch(groups, 1, 1);
		}
		uavBarrier(outRes);
	}
	else
	{
		// BC6H. TryModeG10 covers modes 0..9, TryModeLE10 covers 10..13, then
		// encode. The G10 pass is the seed pass and reads no prior result, but
		// it still binds an in-buffer table to satisfy the root signature.
		runPass(bc6hTryModeG10_.Get(), 0, 16);
		swapBuffers();

		// modes 10..13 map to mode_id 10..13 in candidateModePrec.
		for (uint32_t m = 10; m <= 13; ++m)
		{
			runPass(bc6hTryModeLE10_.Get(), m, 16);
			swapBuffers();
		}

		cl->SetPipelineState(bc6hEncode_.Get());
		cl->SetComputeRootDescriptorTable(kRootSRVBuf, gpuHandle(inSRV));
		cl->SetComputeRootDescriptorTable(kRootUAV, gpuHandle(outUAV));
		for (uint32_t start = 0; start < numTotalBlocks; start += kMaxBlocksPerDispatch)
		{
			const uint32_t count = std::min(kMaxBlocksPerDispatch, numTotalBlocks - start);
			cl->SetComputeRootConstantBufferView(kRootCBV, writeCB(start, 0));
			const uint32_t groups = (count + 3) / 4;
			cl->Dispatch(groups, 1, 1);
		}
		uavBarrier(outRes);
	}

	// Copy the final results (in outRes) to the readback buffer.
	{
		D3D12_RESOURCE_BARRIER toCopy{};
		toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toCopy.Transition.pResource = outRes;
		toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &toCopy);

		cl->CopyBufferRegion(readbackBuffer.Get(), 0, outRes, 0, blockBufferSize);

		D3D12_RESOURCE_BARRIER back{};
		back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		back.Transition.pResource = outRes;
		back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		back.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &back);
	}

	// ---------------------------------------------------------------------
	// 6. Submit and wait. We must block before reading back and before the
	//    upload / constant resources fall out of scope. The queue signals the
	//    fence on GPU completion; fence_->Wait blocks the CPU until then.
	// ---------------------------------------------------------------------
	cmdList_->Close();

	const uint64_t signalValue = ++fenceValue_;
	D3D12CommandList *lists[] = { cmdList_.get() };
	SubmitSyncInfo sync{};
	sync.fence = fence_.get();
	sync.signalValue = signalValue;
	sync.waitValues = nullptr;
	computeQueue_->ExecuteCommandLists(lists, 1, sync);

	fence_->Wait(signalValue);

	// ---------------------------------------------------------------------
	// 7. Copy the compressed blocks out, fixing up the row pitch. Each block is
	//    16 bytes; a row of blocks is numBlockX * 16 bytes, which we place at
	//    the caller's aligned pitch.
	// ---------------------------------------------------------------------
	{
		const uint8_t *mapped = nullptr;
		D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(blockBufferSize) };
		hr = readbackBuffer->Map(0, &readRange, reinterpret_cast<void **>(const_cast<uint8_t **>(&mapped)));
		if (FAILED(hr))
			return hr;

		const size_t srcRowBytes = static_cast<size_t>(numBlockX) * 16;
		for (uint32_t by = 0; by < numBlockY; ++by)
		{
			memcpy(dstPixels + static_cast<size_t>(by) * alignedPitch,
				   mapped + static_cast<size_t>(by) * srcRowBytes,
				   srcRowBytes);
		}

		D3D12_RANGE noWrite{ 0, 0 };
		readbackBuffer->Unmap(0, &noWrite);
	}

	cbResource->Unmap(0, nullptr);

	return S_OK;
}
