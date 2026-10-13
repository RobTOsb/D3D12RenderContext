#pragma once

#include "D3D12CommandList.h"
#include "D3D12CommandQueue.h"
#include "D3D12Device.h"
#include "D3D12Fence.h"

#include "DirectXTex.h"

// Runs BC6H and BC7 block compression on the D3D12 compute queue using the
// BCDirectCompute HLSL shaders (BC6HEncode.hlsl / BC7Encode.hlsl).
// The GPU path only supports BC6H and BC7.  BC4/BC5 must still go through
// the DirectXTex CPU path.
// Usage:
//   GPUEncoder enc(device, computeQueue);
//   enc.Initialise();                                 // compile shaders, build PSOs
//   DirectX::ScratchImage compressed;
//   HRESULT hr = enc.Compress(mipChain, DXGI_FORMAT_BC7_UNORM, compressed);
class GPUEncoder
{
public:
	GPUEncoder(D3D12Device *device, D3D12CommandQueue *computeQueue);
	~GPUEncoder() = default;

	// Compile shaders and build root signatures / PSOs.  Must be called once
	// before Compress().  Returns false on any compilation failure.
	bool Initialise();

	bool IsReady() const
	{
		return ready_;
	}

	// Compresses every mip in srcChain to bcFormat (must be BC6H or BC7).
	// On success, result is filled with the compressed mip chain.
	HRESULT Compress(const DirectX::ScratchImage &srcChain, DXGI_FORMAT bcFormat, DirectX::ScratchImage &result);

private:
	struct EncoderCB
	{
		uint32_t texWidth;
		uint32_t numBlockX;
		uint32_t format;
		uint32_t modeId;
		uint32_t startBlockId;
		uint32_t numTotalBlocks;
		float alphaWeight; // BC7 only; ignored for BC6H
	};

	static constexpr uint32_t kThreadGroupSize = 64;
	static constexpr uint32_t kBlocksPerGroup = kThreadGroupSize / 16; // 4 blocks per group

	// Root parameter indices (shared root signature layout):
	//   [0] CBV  b0  - EncoderCB
	//   [1] SRV  t0  - g_Input  (source Texture2D)
	//   [2] SRV  t1  - g_InBuff (intermediate StructuredBuffer<uint4>)
	//   [3] UAV  u0  - g_OutBuff
	static constexpr uint32_t kRootCBV = 0;
	static constexpr uint32_t kRootSRVTex = 1;
	static constexpr uint32_t kRootSRVBuf = 2;
	static constexpr uint32_t kRootUAV = 3;

	// Views per mip: the source texture, plus an SRV and a UAV for each ping-pong buffer.
	static constexpr uint32_t kDescriptorCount = 5;

	bool CompileShader(const wchar_t *path, const char *entryPoint, Microsoft::WRL::ComPtr<ID3DBlob> &outBlob);

	bool BuildRootSignature();
	bool BuildPSO(ID3DBlob *csBlob, Microsoft::WRL::ComPtr<ID3D12PipelineState> &outPSO);

	// Compress one mip level.  srcTexture is a single-mip SRV, result pixels
	// are written into dstPixels (caller-allocated, size = blockRows * alignedPitch).
	HRESULT CompressMip(const DirectX::Image &srcMip,
						DXGI_FORMAT bcFormat,
						float alphaWeight,
						uint8_t *dstPixels,
						size_t alignedPitch);

	D3D12Device *device_ = nullptr;
	D3D12CommandQueue *computeQueue_ = nullptr;
	bool ready_ = false;

	// The encoder's own shader-visible views (see kDesc* in CompressMip), rewritten for each mip. The
	// renderer's heap only reaches the GPU when it flushes at the start of a frame, so it is not used.
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
	uint32_t descriptorSize_ = 0;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;

	// BC7 passes: TryMode456, TryMode137, TryMode02, EncodeBlock
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc7TryMode456_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc7TryMode137_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc7TryMode02_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc7Encode_;

	// BC6H passes: TryModeG10, TryModeLE10, EncodeBlock
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc6hTryModeG10_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc6hTryModeLE10_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> bc6hEncode_;

	std::unique_ptr<D3D12CommandList> cmdList_;
	std::unique_ptr<D3D12Fence> fence_;
	uint64_t fenceValue_ = 0;
};
