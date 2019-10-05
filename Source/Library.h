#pragma once

#include <stdint.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "EASTL/vector.h"
#include "EASTL/hash_map.h"
#include "DirectXMath/DirectXMath.h"
#include "d3dx12.h"
using namespace DirectX;

#define mz_ASSERT(Expression) { if (!(Expression)) __debugbreak(); }
#define mz_VHR(hr) if (FAILED(hr)) { mz_ASSERT(0); }
#define mz_SAFE_RELEASE(obj) if ((obj)) { (obj)->Release(); (obj) = nullptr; }

#ifdef _DEBUG
#define mz_MALLOC_ALIGNED_OFFSET(Size, Alignment, Offset) _aligned_offset_malloc_dbg((Size), (Alignment), (Offset), __FILE__, __LINE__)
#define mz_FREE(Addr) if ((Addr)) { _aligned_free_dbg((Addr)); }
#else
#define mz_MALLOC_ALIGNED_OFFSET(Size, Alignment, Offset) _aligned_offset_malloc((Size), (Alignment), (Offset))
#define mz_FREE(Addr) if ((Addr)) { _aligned_free_dbg((Addr)); }
#endif
#define mz_MALLOC_ALIGNED(Size, Alignment) mz_MALLOC_ALIGNED_OFFSET((Size), (Alignment), 0)
#define mz_MALLOC(Size) mz_MALLOC_ALIGNED((Size), 8)

struct mz_MeshSection
{
	uint32_t NumVertices;
	uint32_t BaseVertex;
	uint32_t NumIndices;
	uint32_t BaseIndex;
	uint16_t MaterialIndex;
};

struct mz_Mesh
{
	uint16_t NumSections;
	union
	{
		mz_MeshSection Section;
		mz_MeshSection* Sections;
	};
};

struct mz_Material
{
	XMFLOAT4 BaseColorFactor;
	float RoughnessFactor;
	float MetallicFactor;
	uint16_t BaseColorTextureIndex;
	uint16_t PBRFactorsTextureIndex; // Occlusion, Roughness, Metallic.
	uint16_t NormalTextureIndex;
};

struct mz_Object
{
	uint16_t MeshIndex;
	XMFLOAT3X4 ObjectToWorld;
};

struct mz_DX12Resource
{
	ID3D12Resource* Raw;
	D3D12_RESOURCE_STATES State;
	DXGI_FORMAT Format;
};

struct mz_DX12PipelineState
{
	ID3D12PipelineState* PSO;
	ID3D12RootSignature* RS;
};

struct mz_DescriptorHeap
{
	ID3D12DescriptorHeap* Heap;
	D3D12_CPU_DESCRIPTOR_HANDLE CPUStart;
	D3D12_GPU_DESCRIPTOR_HANDLE GPUStart;
	uint32_t Size;
	uint32_t Capacity;
};

struct mz_GPUMemoryHeap
{
	ID3D12Resource* Heap;
	uint8_t* CPUStart;
	D3D12_GPU_VIRTUAL_ADDRESS GPUStart;
	uint32_t Size;
	uint32_t Capacity;
};

struct mz_SceneData
{
	mz_DX12Resource* VertexBuffer;
	mz_DX12Resource* IndexBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE VertexBufferSRV;
	D3D12_CPU_DESCRIPTOR_HANDLE IndexBufferSRV;
	eastl::vector<mz_Mesh> Meshes;
	eastl::vector<mz_Material> Materials;
	eastl::vector<mz_Object> Objects;
	eastl::vector<mz_DX12Resource*> Textures;
	eastl::vector<D3D12_CPU_DESCRIPTOR_HANDLE> TextureSRVs;
};

struct mz_GraphicsContext
{
	ID3D12Device6* Device;
	ID3D12GraphicsCommandList5* CmdList;
	ID3D12CommandQueue* CmdQueue;
	ID3D12CommandAllocator* CmdAlloc[2];
	uint32_t Resolution[2];
	uint32_t DescriptorSize;
	uint32_t DescriptorSizeRTV;
	uint32_t FrameIndex;
	uint32_t BackBufferIndex;
	IDXGISwapChain3* SwapChain;
	mz_DX12Resource* SwapBuffers[4];
	mz_DX12Resource* DepthStencilBuffer;
	mz_DescriptorHeap RTVHeap;
	mz_DescriptorHeap DSVHeap;
	mz_DescriptorHeap CPUDescriptorHeap;
	mz_DescriptorHeap GPUDescriptorHeaps[2];
	mz_GPUMemoryHeap GPUUploadMemoryHeaps[2];
	mz_DX12Resource* Resources;
	mz_DX12PipelineState* Pipelines;
	eastl::hash_map<uint64_t, mz_DX12PipelineState*> GraphicsPipelinesMap;
	eastl::hash_map<uint64_t, mz_DX12PipelineState*> ComputePipelinesMap;
	ID3D12Fence* FrameFence;
	HANDLE FrameFenceEvent;
	uint64_t NumFrames;
	HWND Window;
};

//
// GraphicsContext.
//
mz_GraphicsContext* mz_CreateGraphicsContext(HWND Window, bool bShouldCreateDepthBuffer);
void mz_DestroyGraphicsContext(mz_GraphicsContext* Gfx);
mz_DescriptorHeap* mz_GetDescriptorHeap(mz_GraphicsContext* Gfx, D3D12_DESCRIPTOR_HEAP_TYPE Type, D3D12_DESCRIPTOR_HEAP_FLAGS Flags, uint32_t* OutDescriptorSize);
void mz_PresentFrame(mz_GraphicsContext* Gfx, uint32_t SwapInterval);
void mz_WaitForGPU(mz_GraphicsContext* Gfx);
mz_DX12Resource* mz_AddDX12Resource(mz_GraphicsContext* Gfx, ID3D12Resource* RawResource, D3D12_RESOURCE_STATES InitialState, DXGI_FORMAT Format);
mz_DX12Resource* mz_CreateCommittedResource(mz_GraphicsContext* Gfx, D3D12_HEAP_TYPE HeapType, D3D12_HEAP_FLAGS HeapFlags, D3D12_RESOURCE_DESC* Desc, D3D12_RESOURCE_STATES InitialState, D3D12_CLEAR_VALUE* ClearValue);
mz_DX12PipelineState* mz_CreateGraphicsPipelineState(mz_GraphicsContext* Gfx, D3D12_GRAPHICS_PIPELINE_STATE_DESC* PSODesc, const char* VSName, const char* PSName);
mz_DX12PipelineState* mz_CreateComputePipelineState(mz_GraphicsContext* Gfx, D3D12_COMPUTE_PIPELINE_STATE_DESC* PSODesc, const char* CSName);

//
// Mipmaps.
//
struct mz_MipmapGenerator;
mz_MipmapGenerator* mz_CreateMipmapGenerator(mz_GraphicsContext* Gfx, DXGI_FORMAT Format);
void mz_DestroyMipmapGenerator(mz_MipmapGenerator* Generator);
void mz_GenerateMipmaps(mz_MipmapGenerator* Generator, mz_GraphicsContext* Gfx, mz_DX12Resource* Texture);

//
// UI.
//
struct mz_UIContext;
mz_UIContext* mz_CreateUIContext(mz_GraphicsContext* Gfx, uint32_t NumSamples, eastl::vector<ID3D12Resource*>* OutTempResources);
void mz_DestroyUIContext(mz_UIContext* UI);
void mz_UpdateUI(float DeltaTime);
void mz_DrawUI(mz_UIContext* UI, mz_GraphicsContext* Gfx);

//
// GLTF.
//
void mz_LoadGLTFScene(const char* FileName, mz_GraphicsContext* Gfx, mz_SceneData* OutScene, eastl::vector<ID3D12Resource*>* OutTempResources);

//
// Misc.
//
eastl::vector<uint8_t> mz_LoadFile(const char* Name);
void mz_UpdateFrameStats(HWND Window, const char* Name, double* Time, float* DeltaTime);
double mz_GetTime();
HWND mz_CreateWindow(const char* Name, uint32_t Width, uint32_t Height);


//
// Inline implementation.
//
inline bool
mz_IsValid(mz_DX12Resource* Resource)
{
	return Resource && Resource->Raw != nullptr;
}

inline bool
mz_IsValid(mz_DX12PipelineState* Pipeline)
{
	return Pipeline && Pipeline->PSO != nullptr && Pipeline->RS != nullptr;
}

inline void
mz_ReleaseResource(mz_DX12Resource* Resource)
{
	if (mz_IsValid(Resource) && Resource->Raw->Release() == 0)
	{
		Resource->Raw = nullptr;
		Resource->State = (D3D12_RESOURCE_STATES)~0;
		Resource->Format = (DXGI_FORMAT)~0;
	}
}

inline void
mz_ReleasePipeline(mz_DX12PipelineState* Pipeline)
{
	if (mz_IsValid(Pipeline))
	{
		ULONG PSONumRef = Pipeline->PSO->Release();
		ULONG RSNumRef = Pipeline->RS->Release();
		mz_ASSERT(PSONumRef == RSNumRef);

		if (PSONumRef == 0)
		{
			Pipeline->PSO = nullptr;
			Pipeline->RS = nullptr;
		}
	}
}

inline void
mz_AddTransitionBarrier(mz_DX12Resource* Resource, D3D12_RESOURCE_STATES StateAfter, CD3DX12_RESOURCE_BARRIER* OutBarriers, uint32_t* Num)
{
	if (Resource->State != StateAfter)
	{
		auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(Resource->Raw, Resource->State, StateAfter);
		*OutBarriers = Barrier;
		*Num += 1;
		Resource->State = StateAfter;
	}
}

inline void
mz_CmdResourceBarrier(ID3D12GraphicsCommandList5* CmdList, size_t NumBarriers, D3D12_RESOURCE_BARRIER* Barriers)
{
	if (NumBarriers)
	{
		CmdList->ResourceBarrier((uint32_t)NumBarriers, Barriers);
	}
}

inline void
mz_CmdTransitionBarrier(ID3D12GraphicsCommandList5* CmdList, mz_DX12Resource* Resource, D3D12_RESOURCE_STATES StateAfter)
{
	if (Resource->State != StateAfter)
	{
		auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(Resource->Raw, Resource->State, StateAfter);
		CmdList->ResourceBarrier(1, &Barrier);
		Resource->State = StateAfter;
	}
}

inline ID3D12GraphicsCommandList5*
mz_CmdInit(mz_GraphicsContext* Gfx)
{
	Gfx->CmdAlloc[Gfx->FrameIndex]->Reset();
	Gfx->CmdList->Reset(Gfx->CmdAlloc[Gfx->FrameIndex], nullptr);
	Gfx->CmdList->SetDescriptorHeaps(1, &Gfx->GPUDescriptorHeaps[Gfx->FrameIndex].Heap);
	return Gfx->CmdList;
}

inline D3D12_CPU_DESCRIPTOR_HANDLE
mz_AllocateDescriptors(mz_GraphicsContext* Gfx, D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t Count)
{
	uint32_t DescriptorSize;
	mz_DescriptorHeap* Heap = mz_GetDescriptorHeap(Gfx, Type, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &DescriptorSize);
	mz_ASSERT((Heap->Size + Count) < Heap->Capacity);

	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle;
	CPUHandle.ptr = Heap->CPUStart.ptr + (size_t)Heap->Size * DescriptorSize;
	Heap->Size += Count;

	return CPUHandle;
}

inline void
mz_AllocateGPUDescriptors(mz_GraphicsContext* Gfx, uint32_t Count, D3D12_CPU_DESCRIPTOR_HANDLE* OutCPUHandle, D3D12_GPU_DESCRIPTOR_HANDLE* OutGPUHandle)
{
	uint32_t DescriptorSize;
	mz_DescriptorHeap* Heap = mz_GetDescriptorHeap(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &DescriptorSize);
	mz_ASSERT((Heap->Size + Count) < Heap->Capacity);

	OutCPUHandle->ptr = Heap->CPUStart.ptr + (size_t)Heap->Size * DescriptorSize;
	OutGPUHandle->ptr = Heap->GPUStart.ptr + (size_t)Heap->Size * DescriptorSize;

	Heap->Size += Count;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE
mz_CopyDescriptorsToGPUHeap(mz_GraphicsContext* Gfx, uint32_t Count, D3D12_CPU_DESCRIPTOR_HANDLE SrcBaseHandle)
{
	D3D12_CPU_DESCRIPTOR_HANDLE CPUBaseHandle;
	D3D12_GPU_DESCRIPTOR_HANDLE GPUBaseHandle;
	mz_AllocateGPUDescriptors(Gfx, Count, &CPUBaseHandle, &GPUBaseHandle);
	Gfx->Device->CopyDescriptorsSimple(Count, CPUBaseHandle, SrcBaseHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return GPUBaseHandle;
}

inline void*
mz_AllocateGPUMemory(mz_GraphicsContext* Gfx, uint32_t Size, D3D12_GPU_VIRTUAL_ADDRESS* OutGPUAddress)
{
	mz_ASSERT(Size > 0);

	if (Size & 0xff)
	{
		// Always align to 256 bytes.
		Size = (Size + 255) & ~0xff;
	}

	mz_GPUMemoryHeap& UploadHeap = Gfx->GPUUploadMemoryHeaps[Gfx->FrameIndex];
	mz_ASSERT((UploadHeap.Size + Size) < UploadHeap.Capacity);

	void* CPUAddress = UploadHeap.CPUStart + UploadHeap.Size;
	*OutGPUAddress = UploadHeap.GPUStart + UploadHeap.Size;

	UploadHeap.Size += Size;
	return CPUAddress;
}

inline void
mz_GetBackBuffer(mz_GraphicsContext* Gfx, mz_DX12Resource** OutBuffer, D3D12_CPU_DESCRIPTOR_HANDLE* OutHandle)
{
	mz_ASSERT(OutBuffer && OutHandle);
	*OutBuffer = Gfx->SwapBuffers[Gfx->BackBufferIndex];
	*OutHandle = Gfx->RTVHeap.CPUStart;
	OutHandle->ptr += Gfx->BackBufferIndex * (size_t)Gfx->DescriptorSizeRTV;
}

inline void
mz_GetDepthStencilBuffer(mz_GraphicsContext* Gfx, mz_DX12Resource** OutBuffer, D3D12_CPU_DESCRIPTOR_HANDLE* OutHandle)
{
	mz_ASSERT(OutBuffer && OutHandle);
	mz_ASSERT(Gfx->DepthStencilBuffer);
	*OutBuffer = Gfx->DepthStencilBuffer;
	*OutHandle = Gfx->DSVHeap.CPUStart;
}

inline mz_MeshSection*
mz_GetMeshSections(mz_Mesh* Mesh)
{
	mz_ASSERT(Mesh->NumSections > 0);
	if (Mesh->NumSections == 1)
	{
		return &Mesh->Section;
	}
	mz_ASSERT(Mesh->Sections);
	return Mesh->Sections;
}

inline void
mz_DestroyMesh(mz_Mesh* Mesh)
{
	mz_ASSERT(Mesh->NumSections > 0);
	if (Mesh->Sections && Mesh->NumSections > 1)
	{
		free(Mesh->Sections);
		Mesh->Sections = nullptr;
	}
	Mesh->NumSections = 0;
}

template <typename T> inline bool
mz_IsPowerOf2(T X)
{
	return (X & (X - 1)) == 0;
}
