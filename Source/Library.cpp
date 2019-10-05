#include "Library.h"
#include <stdio.h>
#include "d3dx12.h"
#include "imgui/imgui.h"
#include "meow_hash_x64_aesni.h"
#include "cgltf.h"
#include "stb_image.h"
#include "CPUAndGPUCommon.h"

#define mz_MAX_NUM_RESOURCES 256
#define mz_MAX_NUM_PIPELINES 64

struct mz_UIFrameResources
{
	ID3D12Resource* VertexBuffer;
	ID3D12Resource* IndexBuffer;
	void* VertexBufferCPUAddress;
	void* IndexBufferCPUAddress;
	uint32_t VertexBufferSize;
	uint32_t IndexBufferSize;
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView;
	D3D12_INDEX_BUFFER_VIEW IndexBufferView;
};

struct mz_UIContext
{
	mz_DX12PipelineState* PipelineState;
	ID3D12Resource* Font;
	D3D12_CPU_DESCRIPTOR_HANDLE FontSRV;
	mz_UIFrameResources Frames[2];
};

struct mz_MipmapGenerator
{
	mz_DX12PipelineState* Pipeline;
	mz_DX12Resource* ScratchTextures[4];
	D3D12_CPU_DESCRIPTOR_HANDLE ScratchTexturesBaseUAV;
	DXGI_FORMAT Format;
};

static void mz_CreateHeaps(mz_GraphicsContext* Gfx);

void* operator new(size_t Size)
{
	return mz_MALLOC(Size);
}

void* operator new[](size_t Size)
{
	return mz_MALLOC(Size);
}

void operator delete(void* P)
{
	mz_FREE(P);
}

void operator delete[](void* P)
{
	mz_FREE(P);
}

void* operator new[](size_t Size, const char* /*Name*/, int /*Flags*/, unsigned /*DebugFlags*/, const char* /*File*/, int /*Line*/)
{
	return mz_MALLOC(Size);
}

void* operator new[](size_t Size, size_t Alignment, size_t AlignmentOffset, const char* /*Name*/, int /*Flags*/, unsigned /*DebugFlags*/, const char* /*File*/, int /*Line*/)
{
	return mz_MALLOC_ALIGNED_OFFSET(Size, Alignment, AlignmentOffset);
}

mz_GraphicsContext*
mz_CreateGraphicsContext(HWND Window, bool bShouldCreateDepthBuffer)
{
	static mz_GraphicsContext Gfx = {};

	if (Gfx.Device != nullptr)
	{
		return &Gfx;
	}

	IDXGIFactory4* Factory;
#ifdef _DEBUG
	mz_VHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&Factory)));
#else
	mz_VHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory)));
#endif
#ifdef _DEBUG
	{
		ID3D12Debug* Dbg;
		D3D12GetDebugInterface(IID_PPV_ARGS(&Dbg));
		if (Dbg)
		{
			Dbg->EnableDebugLayer();
			ID3D12Debug1* Dbg1;
			Dbg->QueryInterface(IID_PPV_ARGS(&Dbg1));
			if (Dbg1)
			{
				Dbg1->SetEnableGPUBasedValidation(FALSE);
			}
			mz_SAFE_RELEASE(Dbg);
			mz_SAFE_RELEASE(Dbg1);
		}
	}
#endif
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&Gfx.Device))))
	{
		MessageBox(Window, "This application requires Windows 10 (May 2019) or newer and GPU with raytracing support.", "D3D12CreateDevice failed", MB_OK | MB_ICONERROR);
		exit(0);
	}

	Gfx.Window = Window;

	D3D12_COMMAND_QUEUE_DESC CmdQueueDesc = {};
	CmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	CmdQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	CmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	mz_VHR(Gfx.Device->CreateCommandQueue(&CmdQueueDesc, IID_PPV_ARGS(&Gfx.CmdQueue)));

	DXGI_SWAP_CHAIN_DESC SwapChainDesc = {};
	SwapChainDesc.BufferCount = 4;
	SwapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	SwapChainDesc.OutputWindow = Window;
	SwapChainDesc.SampleDesc.Count = 1;
	SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	SwapChainDesc.Windowed = TRUE;

	IDXGISwapChain* TempSwapChain;
	mz_VHR(Factory->CreateSwapChain(Gfx.CmdQueue, &SwapChainDesc, &TempSwapChain));
	mz_VHR(TempSwapChain->QueryInterface(IID_PPV_ARGS(&Gfx.SwapChain)));
	mz_SAFE_RELEASE(TempSwapChain);
	mz_SAFE_RELEASE(Factory);

	Gfx.Resources = (mz_DX12Resource*)mz_MALLOC(mz_MAX_NUM_RESOURCES * sizeof(mz_DX12Resource));
	memset(Gfx.Resources, 0, mz_MAX_NUM_RESOURCES * sizeof(mz_DX12Resource));

	Gfx.Pipelines = (mz_DX12PipelineState*)mz_MALLOC(mz_MAX_NUM_PIPELINES * sizeof(mz_DX12PipelineState));
	memset(Gfx.Pipelines, 0, mz_MAX_NUM_PIPELINES * sizeof(mz_DX12PipelineState));

	RECT Rect;
	GetClientRect(Window, &Rect);
	Gfx.Resolution[0] = (uint32_t)Rect.right;
	Gfx.Resolution[1] = (uint32_t)Rect.bottom;

	for (uint32_t Idx = 0; Idx < 2; ++Idx)
	{
		mz_VHR(Gfx.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Gfx.CmdAlloc[Idx])));
	}

	Gfx.DescriptorSize = Gfx.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	Gfx.DescriptorSizeRTV = Gfx.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	mz_CreateHeaps(&Gfx);

	// Swap-buffer render targets.
	{
		D3D12_CPU_DESCRIPTOR_HANDLE Handle = mz_AllocateDescriptors(&Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4);

		for (uint32_t Idx = 0; Idx < 4; ++Idx)
		{
			{
				ID3D12Resource* Buffer;
				mz_VHR(Gfx.SwapChain->GetBuffer(Idx, IID_PPV_ARGS(&Buffer)));
				Gfx.SwapBuffers[Idx] = mz_AddDX12Resource(&Gfx, Buffer, D3D12_RESOURCE_STATE_PRESENT, SwapChainDesc.BufferDesc.Format);
			}
			Gfx.Device->CreateRenderTargetView(Gfx.SwapBuffers[Idx]->Raw, nullptr, Handle);
			Handle.ptr += Gfx.DescriptorSizeRTV;
		}
	}

	// Depth-stencil target.
	if (bShouldCreateDepthBuffer)
	{
		auto ImageDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, Gfx.Resolution[0], Gfx.Resolution[1], 1, 1);
		ImageDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

		Gfx.DepthStencilBuffer = mz_CreateCommittedResource(&Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &ImageDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &CD3DX12_CLEAR_VALUE(DXGI_FORMAT_D32_FLOAT, 1.0f, 0));

		D3D12_CPU_DESCRIPTOR_HANDLE Handle = mz_AllocateDescriptors(&Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

		D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc = {};
		ViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
		ViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		ViewDesc.Flags = D3D12_DSV_FLAG_NONE;
		Gfx.Device->CreateDepthStencilView(Gfx.DepthStencilBuffer->Raw, &ViewDesc, Handle);
	}

	mz_VHR(Gfx.Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Gfx.CmdAlloc[0], nullptr, IID_PPV_ARGS(&Gfx.CmdList)));
	mz_VHR(Gfx.CmdList->Close());

	mz_VHR(Gfx.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Gfx.FrameFence)));
	Gfx.FrameFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

	mz_CmdInit(&Gfx);

	return &Gfx;
}

void
mz_DestroyGraphicsContext(mz_GraphicsContext* Gfx)
{
	mz_ASSERT(Gfx);
	CloseHandle(Gfx->FrameFenceEvent);
	for (uint32_t Idx = 0; Idx < mz_MAX_NUM_RESOURCES; ++Idx)
	{
		mz_ReleaseResource(&Gfx->Resources[Idx]);
	}
	for (uint32_t Idx = 0; Idx < mz_MAX_NUM_PIPELINES; ++Idx)
	{
		mz_SAFE_RELEASE(Gfx->Pipelines[Idx].PSO);
		mz_SAFE_RELEASE(Gfx->Pipelines[Idx].RS);
	}
	Gfx->GraphicsPipelinesMap.clear();
	Gfx->ComputePipelinesMap.clear();
	mz_SAFE_RELEASE(Gfx->CmdList);
	mz_SAFE_RELEASE(Gfx->RTVHeap.Heap);
	mz_SAFE_RELEASE(Gfx->DSVHeap.Heap);
	for (uint32_t Idx = 0; Idx < 2; ++Idx)
	{
		mz_SAFE_RELEASE(Gfx->CmdAlloc[Idx]);
		mz_SAFE_RELEASE(Gfx->GPUDescriptorHeaps[Idx].Heap);
		mz_SAFE_RELEASE(Gfx->GPUUploadMemoryHeaps[Idx].Heap);
	}
	mz_SAFE_RELEASE(Gfx->CPUDescriptorHeap.Heap);
	mz_ReleaseResource(Gfx->DepthStencilBuffer);
	mz_SAFE_RELEASE(Gfx->FrameFence);
	mz_SAFE_RELEASE(Gfx->SwapChain);
	mz_SAFE_RELEASE(Gfx->CmdQueue);
	mz_SAFE_RELEASE(Gfx->Device);
	mz_FREE(Gfx->Pipelines);
	mz_FREE(Gfx->Resources);
}

void
mz_PresentFrame(mz_GraphicsContext* Gfx, uint32_t SwapInterval)
{
	Gfx->SwapChain->Present(SwapInterval, 0);
	Gfx->CmdQueue->Signal(Gfx->FrameFence, ++Gfx->NumFrames);

	uint64_t GPUFrameCount = Gfx->FrameFence->GetCompletedValue();

	if ((Gfx->NumFrames - GPUFrameCount) >= 2)
	{
		Gfx->FrameFence->SetEventOnCompletion(GPUFrameCount + 1, Gfx->FrameFenceEvent);
		WaitForSingleObject(Gfx->FrameFenceEvent, INFINITE);
	}

	Gfx->FrameIndex = !Gfx->FrameIndex;
	Gfx->BackBufferIndex = Gfx->SwapChain->GetCurrentBackBufferIndex();
	Gfx->GPUDescriptorHeaps[Gfx->FrameIndex].Size = 0;
	Gfx->GPUUploadMemoryHeaps[Gfx->FrameIndex].Size = 0;
}

void
mz_WaitForGPU(mz_GraphicsContext* Gfx)
{
	Gfx->CmdQueue->Signal(Gfx->FrameFence, ++Gfx->NumFrames);
	Gfx->FrameFence->SetEventOnCompletion(Gfx->NumFrames, Gfx->FrameFenceEvent);
	WaitForSingleObject(Gfx->FrameFenceEvent, INFINITE);

	Gfx->GPUDescriptorHeaps[Gfx->FrameIndex].Size = 0;
	Gfx->GPUUploadMemoryHeaps[Gfx->FrameIndex].Size = 0;
}

mz_DescriptorHeap*
mz_GetDescriptorHeap(mz_GraphicsContext* Gfx, D3D12_DESCRIPTOR_HEAP_TYPE Type, D3D12_DESCRIPTOR_HEAP_FLAGS Flags, uint32_t* DescriptorSize)
{
	if (Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
	{
		*DescriptorSize = Gfx->DescriptorSizeRTV;
		return &Gfx->RTVHeap;
	}
	else if (Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
	{
		*DescriptorSize = Gfx->DescriptorSizeRTV;
		return &Gfx->DSVHeap;
	}
	else if (Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
	{
		*DescriptorSize = Gfx->DescriptorSize;
		if (Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
		{
			return &Gfx->CPUDescriptorHeap;
		}
		else if (Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
		{
			return &Gfx->GPUDescriptorHeaps[Gfx->FrameIndex];
		}
	}
	mz_ASSERT(0);
	*DescriptorSize = 0;
	return &Gfx->CPUDescriptorHeap;
}

static void
mz_CreateHeaps(mz_GraphicsContext* Gfx)
{
	// Render target descriptor heap (RTV).
	{
		Gfx->RTVHeap.Capacity = 1024;

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = Gfx->RTVHeap.Capacity;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		mz_VHR(Gfx->Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&Gfx->RTVHeap.Heap)));
		Gfx->RTVHeap.CPUStart = Gfx->RTVHeap.Heap->GetCPUDescriptorHandleForHeapStart();
	}
	// Depth-stencil descriptor heap (DSV).
	{
		Gfx->DSVHeap.Capacity = 1024;

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = Gfx->DSVHeap.Capacity;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		mz_VHR(Gfx->Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&Gfx->DSVHeap.Heap)));
		Gfx->DSVHeap.CPUStart = Gfx->DSVHeap.Heap->GetCPUDescriptorHandleForHeapStart();
	}
	// Non-shader visible descriptor heap (CBV, SRV, UAV).
	{
		Gfx->CPUDescriptorHeap.Capacity = 16 * 1024;

		D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
		HeapDesc.NumDescriptors = Gfx->CPUDescriptorHeap.Capacity;
		HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		mz_VHR(Gfx->Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&Gfx->CPUDescriptorHeap.Heap)));
		Gfx->CPUDescriptorHeap.CPUStart = Gfx->CPUDescriptorHeap.Heap->GetCPUDescriptorHandleForHeapStart();
	}
	// Shader visible descriptor heaps (CBV, SRV, UAV).
	{
		for (uint32_t Idx = 0; Idx < 2; ++Idx)
		{
			mz_DescriptorHeap& Heap = Gfx->GPUDescriptorHeaps[Idx];
			Heap.Capacity = 16 * 1024;

			D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
			HeapDesc.NumDescriptors = Heap.Capacity;
			HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			mz_VHR(Gfx->Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(&Heap.Heap)));

			Heap.CPUStart = Heap.Heap->GetCPUDescriptorHandleForHeapStart();
			Heap.GPUStart = Heap.Heap->GetGPUDescriptorHandleForHeapStart();
		}
	}
	// Upload Memory Heaps.
	{
		for (uint32_t Index = 0; Index < 2; ++Index)
		{
			mz_GPUMemoryHeap& UploadHeap = Gfx->GPUUploadMemoryHeaps[Index];
			UploadHeap.Size = 0;
			UploadHeap.Capacity = 8 * 1024 * 1024;
			UploadHeap.CPUStart = nullptr;
			UploadHeap.GPUStart = 0;

			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(UploadHeap.Capacity), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadHeap.Heap)));

			mz_VHR(UploadHeap.Heap->Map(0, &CD3DX12_RANGE(0, 0), (void**)&UploadHeap.CPUStart));
			UploadHeap.GPUStart = UploadHeap.Heap->GetGPUVirtualAddress();
		}
	}
}

mz_UIContext*
mz_CreateUIContext(mz_GraphicsContext* Gfx, uint32_t NumSamples, eastl::vector<ID3D12Resource*>* OutTempResources)
{
	// NOTE(mziulek): Only one UI context can exist.
	static mz_UIContext UI = {};

	if (UI.PipelineState != nullptr)
	{
		return &UI;
	}

	ImGuiIO* IO = &ImGui::GetIO();
	IO->KeyMap[ImGuiKey_Tab] = VK_TAB;
	IO->KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
	IO->KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
	IO->KeyMap[ImGuiKey_UpArrow] = VK_UP;
	IO->KeyMap[ImGuiKey_DownArrow] = VK_DOWN;
	IO->KeyMap[ImGuiKey_PageUp] = VK_PRIOR;
	IO->KeyMap[ImGuiKey_PageDown] = VK_NEXT;
	IO->KeyMap[ImGuiKey_Home] = VK_HOME;
	IO->KeyMap[ImGuiKey_End] = VK_END;
	IO->KeyMap[ImGuiKey_Delete] = VK_DELETE;
	IO->KeyMap[ImGuiKey_Backspace] = VK_BACK;
	IO->KeyMap[ImGuiKey_Enter] = VK_RETURN;
	IO->KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
	IO->KeyMap[ImGuiKey_A] = 'A';
	IO->KeyMap[ImGuiKey_C] = 'C';
	IO->KeyMap[ImGuiKey_V] = 'V';
	IO->KeyMap[ImGuiKey_X] = 'X';
	IO->KeyMap[ImGuiKey_Y] = 'Y';
	IO->KeyMap[ImGuiKey_Z] = 'Z';
	IO->ImeWindowHandle = Gfx->Window;
	IO->RenderDrawListsFn = nullptr;
	IO->DisplaySize = ImVec2((float)Gfx->Resolution[0], (float)Gfx->Resolution[1]);
	ImGui::GetStyle().WindowRounding = 0.0f;

	uint8_t* Pixels;
	int32_t Width, Height;
	IO->Fonts->AddFontFromFileTTF("Data/Roboto-Medium.ttf", 18.0f);
	IO->Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);

	mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, Width, Height, 1, 1), D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&UI.Font)));
	{
		auto BufferDesc = CD3DX12_RESOURCE_DESC::Buffer(GetRequiredIntermediateSize(UI.Font, 0, 1));
		ID3D12Resource* StagingBuffer;
		mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &BufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingBuffer)));
		OutTempResources->push_back(StagingBuffer);

		D3D12_SUBRESOURCE_DATA TextureData = { Pixels, (LONG_PTR)Width * 4 };
		UpdateSubresources<1>(Gfx->CmdList, UI.Font, StagingBuffer, 0, 0, 1, &TextureData);

		Gfx->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(UI.Font, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	SRVDesc.Texture2D.MipLevels = 1;

	UI.FontSRV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
	Gfx->Device->CreateShaderResourceView(UI.Font, &SRVDesc, UI.FontSRV);

	D3D12_INPUT_ELEMENT_DESC InputElements[] =
	{
		{ "_Position", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "_Texcoords", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "_Color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	eastl::vector<uint8_t> VSBytecode = mz_LoadFile("Data/Shaders/UserInterface.vs.cso");
	eastl::vector<uint8_t> PSBytecode = mz_LoadFile("Data/Shaders/UserInterface.ps.cso");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc = {};
	PSODesc.InputLayout = { InputElements, (uint32_t)eastl::size(InputElements) };
	PSODesc.VS = { VSBytecode.data(), VSBytecode.size() };
	PSODesc.PS = { PSBytecode.data(), PSBytecode.size() };
	PSODesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	PSODesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	PSODesc.RasterizerState.MultisampleEnable = NumSamples > 1 ? TRUE : FALSE;
	PSODesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	PSODesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	PSODesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	PSODesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	PSODesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	PSODesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	PSODesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	PSODesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	PSODesc.SampleMask = UINT_MAX;
	PSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	PSODesc.NumRenderTargets = 1;
	PSODesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	PSODesc.SampleDesc.Count = NumSamples;

	UI.PipelineState = mz_CreateGraphicsPipelineState(Gfx, &PSODesc, "UserInterface.vs.cso", "UserInterface.ps.cso");

	return &UI;
}

void
mz_DestroyUIContext(mz_UIContext* UI)
{
	mz_ASSERT(UI);
	mz_SAFE_RELEASE(UI->Font);
	for (uint32_t Idx = 0; Idx < eastl::size(UI->Frames); ++Idx)
	{
		mz_SAFE_RELEASE(UI->Frames[Idx].VertexBuffer);
		mz_SAFE_RELEASE(UI->Frames[Idx].IndexBuffer);
	}
}

void
mz_UpdateUI(float DeltaTime)
{
	ImGuiIO* IO = &ImGui::GetIO();
	IO->KeyCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	IO->KeyShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	IO->KeyAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
	IO->DeltaTime = DeltaTime;

	ImGui::NewFrame();
}

void
mz_DrawUI(mz_UIContext* UI, mz_GraphicsContext* Gfx)
{
	mz_ASSERT(UI && Gfx);

	ImGui::Render();

	ImDrawData* DrawData = ImGui::GetDrawData();
	if (!DrawData || DrawData->TotalVtxCount == 0)
	{
		return;
	}

	ImGuiIO* IO = &ImGui::GetIO();
	mz_UIFrameResources& Frame = UI->Frames[Gfx->FrameIndex];

	auto ViewportWidth = (int32_t)(IO->DisplaySize.x * IO->DisplayFramebufferScale.x);
	auto ViewportHeight = (int32_t)(IO->DisplaySize.y * IO->DisplayFramebufferScale.y);
	DrawData->ScaleClipRects(IO->DisplayFramebufferScale);

	// Create or resize vertex buffer if needed.
	if (Frame.VertexBufferSize == 0 || Frame.VertexBufferSize < DrawData->TotalVtxCount * sizeof(ImDrawVert))
	{
		mz_SAFE_RELEASE(Frame.VertexBuffer);
		mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(DrawData->TotalVtxCount * sizeof(ImDrawVert)), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Frame.VertexBuffer)));

		mz_VHR(Frame.VertexBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Frame.VertexBufferCPUAddress));

		Frame.VertexBufferSize = DrawData->TotalVtxCount * sizeof(ImDrawVert);

		Frame.VertexBufferView.BufferLocation = Frame.VertexBuffer->GetGPUVirtualAddress();
		Frame.VertexBufferView.StrideInBytes = sizeof(ImDrawVert);
		Frame.VertexBufferView.SizeInBytes = DrawData->TotalVtxCount * sizeof(ImDrawVert);
	}

	// Create or resize index buffer if needed.
	if (Frame.IndexBufferSize == 0 || Frame.IndexBufferSize < DrawData->TotalIdxCount * sizeof(ImDrawIdx))
	{
		mz_SAFE_RELEASE(Frame.IndexBuffer);
		mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(DrawData->TotalIdxCount * sizeof(ImDrawIdx)), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Frame.IndexBuffer)));

		mz_VHR(Frame.IndexBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Frame.IndexBufferCPUAddress));

		Frame.IndexBufferSize = DrawData->TotalIdxCount * sizeof(ImDrawIdx);

		Frame.IndexBufferView.BufferLocation = Frame.IndexBuffer->GetGPUVirtualAddress();
		Frame.IndexBufferView.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
		Frame.IndexBufferView.SizeInBytes = DrawData->TotalIdxCount * sizeof(ImDrawIdx);
	}

	// Update vertex and index buffers.
	{
		ImDrawVert* VertexPtr = (ImDrawVert*)Frame.VertexBufferCPUAddress;
		ImDrawIdx* IndexPtr = (ImDrawIdx*)Frame.IndexBufferCPUAddress;

		for (uint32_t CmdListIdx = 0; CmdListIdx < (uint32_t)DrawData->CmdListsCount; ++CmdListIdx)
		{
			ImDrawList* DrawList = DrawData->CmdLists[CmdListIdx];
			memcpy(VertexPtr, &DrawList->VtxBuffer[0], DrawList->VtxBuffer.size() * sizeof(ImDrawVert));
			memcpy(IndexPtr, &DrawList->IdxBuffer[0], DrawList->IdxBuffer.size() * sizeof(ImDrawIdx));
			VertexPtr += DrawList->VtxBuffer.size();
			IndexPtr += DrawList->IdxBuffer.size();
		}
	}

	D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferGPUAddress;
	auto ConstantBufferCPUAddress = (XMFLOAT4X4*)mz_AllocateGPUMemory(Gfx, 64, &ConstantBufferGPUAddress);

	// Update constant buffer.
	{
		XMMATRIX M = XMMatrixTranspose(XMMatrixOrthographicOffCenterLH(0.0f, (float)ViewportWidth, (float)ViewportHeight, 0.0f, 0.0f, 1.0f));
		XMStoreFloat4x4(ConstantBufferCPUAddress, M);
	}

	ID3D12GraphicsCommandList2* CmdList = Gfx->CmdList;

	CmdList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, (float)ViewportWidth, (float)ViewportHeight));

	CmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CmdList->SetPipelineState(UI->PipelineState->PSO);

	CmdList->SetGraphicsRootSignature(UI->PipelineState->RS);
	CmdList->SetGraphicsRootConstantBufferView(0, ConstantBufferGPUAddress);
	CmdList->SetGraphicsRootDescriptorTable(1, mz_CopyDescriptorsToGPUHeap(Gfx, 1, UI->FontSRV));

	CmdList->IASetVertexBuffers(0, 1, &Frame.VertexBufferView);
	CmdList->IASetIndexBuffer(&Frame.IndexBufferView);


	int32_t VertexOffset = 0;
	uint32_t IndexOffset = 0;
	for (uint32_t CmdListIdx = 0; CmdListIdx < (uint32_t)DrawData->CmdListsCount; ++CmdListIdx)
	{
		ImDrawList* DrawList = DrawData->CmdLists[CmdListIdx];

		for (uint32_t CmdIndex = 0; CmdIndex < (uint32_t)DrawList->CmdBuffer.size(); ++CmdIndex)
		{
			ImDrawCmd* Cmd = &DrawList->CmdBuffer[CmdIndex];

			if (Cmd->UserCallback)
			{
				Cmd->UserCallback(DrawList, Cmd);
			}
			else
			{
				D3D12_RECT R = CD3DX12_RECT((LONG)Cmd->ClipRect.x, (LONG)Cmd->ClipRect.y, (LONG)Cmd->ClipRect.z, (LONG)Cmd->ClipRect.w);
				CmdList->RSSetScissorRects(1, &R);
				CmdList->DrawIndexedInstanced(Cmd->ElemCount, 1, IndexOffset, VertexOffset, 0);
			}
			IndexOffset += Cmd->ElemCount;
		}
		VertexOffset += DrawList->VtxBuffer.size();
	}
}

mz_MipmapGenerator*
mz_CreateMipmapGenerator(mz_GraphicsContext* Gfx, DXGI_FORMAT Format)
{
	// We will support textures up to 2048x2048 for now.

	mz_MipmapGenerator* Generator = (mz_MipmapGenerator*)calloc(1, sizeof(mz_MipmapGenerator));
	if (Generator == nullptr)
	{
		return nullptr;
	}

	uint32_t Width = 2048 / 2;
	uint32_t Height = 2048 / 2;

	for (uint32_t Idx = 0; Idx < eastl::size(Generator->ScratchTextures); ++Idx)
	{
		auto TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(Format, Width, Height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		Generator->ScratchTextures[Idx] = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &TextureDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);

		Width /= 2;
		Height /= 2;
	}

	Generator->ScratchTexturesBaseUAV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);

	D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = Generator->ScratchTexturesBaseUAV;
	for (uint32_t Idx = 0; Idx < eastl::size(Generator->ScratchTextures); ++Idx)
	{
		Gfx->Device->CreateUnorderedAccessView(Generator->ScratchTextures[Idx]->Raw, nullptr, nullptr, CPUHandle);
		CPUHandle.ptr += Gfx->DescriptorSize;
	}

	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc = {};
		Generator->Pipeline = mz_CreateComputePipelineState(Gfx, &PSODesc, "GenerateMipmaps.cs.cso");
	}

	Generator->Format = Format;
	return Generator;
}

void
mz_DestroyMipmapGenerator(mz_MipmapGenerator* Generator)
{
	mz_ASSERT(Generator);
	for (uint32_t Idx = 0; Idx < eastl::size(Generator->ScratchTextures); ++Idx)
	{
		mz_ReleaseResource(Generator->ScratchTextures[Idx]);
	}
}

void
mz_GenerateMipmaps(mz_MipmapGenerator* Generator, mz_GraphicsContext* Gfx, mz_DX12Resource* Texture)
{
	mz_ASSERT(Texture && Texture->State == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	mz_ASSERT(Texture->Format == Generator->Format);

	D3D12_RESOURCE_DESC TextureDesc = Texture->Raw->GetDesc();

	mz_ASSERT(TextureDesc.Width <= 2048 && TextureDesc.Height <= 2048);
	mz_ASSERT(TextureDesc.Width == TextureDesc.Height);
	mz_ASSERT(mz_IsPowerOf2(TextureDesc.Width) && mz_IsPowerOf2(TextureDesc.Height));
	mz_ASSERT(TextureDesc.MipLevels > 1);

	ID3D12GraphicsCommandList5* CmdList = Gfx->CmdList;


	for (uint32_t ArraySliceIdx = 0; ArraySliceIdx < TextureDesc.DepthOrArraySize; ++ArraySliceIdx)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE TextureSRV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		SRVDesc.Texture2DArray.MostDetailedMip = 0;
		SRVDesc.Texture2DArray.MipLevels = TextureDesc.MipLevels;
		SRVDesc.Texture2DArray.ArraySize = 1;
		SRVDesc.Texture2DArray.FirstArraySlice = ArraySliceIdx;
		Gfx->Device->CreateShaderResourceView(Texture->Raw, &SRVDesc, TextureSRV);

		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle = mz_CopyDescriptorsToGPUHeap(Gfx, 1, TextureSRV);
		mz_CopyDescriptorsToGPUHeap(Gfx, 4, Generator->ScratchTexturesBaseUAV);

		CmdList->SetPipelineState(Generator->Pipeline->PSO);
		CmdList->SetComputeRootSignature(Generator->Pipeline->RS);

		uint32_t TotalNumMipsToGen = (uint32_t)(TextureDesc.MipLevels - 1);
		uint32_t CurrentSrcMipLevel = 0;

		for (;;)
		{
			uint32_t NumMipsInDispatch = TotalNumMipsToGen >= 4 ? 4 : TotalNumMipsToGen;

			CmdList->SetComputeRoot32BitConstant(0, CurrentSrcMipLevel, 0);
			CmdList->SetComputeRoot32BitConstant(0, NumMipsInDispatch, 1);
			CmdList->SetComputeRootDescriptorTable(1, GPUHandle);
			CmdList->Dispatch((uint32_t)(TextureDesc.Width >> (4 + CurrentSrcMipLevel)), TextureDesc.Height >> (4 + CurrentSrcMipLevel), 1);

			{
				uint32_t NumBarriers = 0;
				CD3DX12_RESOURCE_BARRIER Barriers[5];
				mz_AddTransitionBarrier(Generator->ScratchTextures[0], D3D12_RESOURCE_STATE_COPY_SOURCE, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[1], D3D12_RESOURCE_STATE_COPY_SOURCE, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[2], D3D12_RESOURCE_STATE_COPY_SOURCE, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[3], D3D12_RESOURCE_STATE_COPY_SOURCE, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Texture, D3D12_RESOURCE_STATE_COPY_DEST, &Barriers[NumBarriers], &NumBarriers);
				mz_CmdResourceBarrier(CmdList, NumBarriers, Barriers);
			}

			for (uint32_t MipmapIdx = 0; MipmapIdx < NumMipsInDispatch; ++MipmapIdx)
			{
				auto Dest = CD3DX12_TEXTURE_COPY_LOCATION(Texture->Raw, MipmapIdx + 1 + CurrentSrcMipLevel + ArraySliceIdx * TextureDesc.MipLevels);
				auto Src = CD3DX12_TEXTURE_COPY_LOCATION(Generator->ScratchTextures[MipmapIdx]->Raw, 0);
				auto Box = CD3DX12_BOX(0, 0, 0, (UINT)(TextureDesc.Width >> (MipmapIdx + 1 + CurrentSrcMipLevel)), TextureDesc.Height >> (MipmapIdx + 1 + CurrentSrcMipLevel), 1);
				CmdList->CopyTextureRegion(&Dest, 0, 0, 0, &Src, &Box);
			}

			{
				uint32_t NumBarriers = 0;
				CD3DX12_RESOURCE_BARRIER Barriers[5];
				mz_AddTransitionBarrier(Generator->ScratchTextures[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[2], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Generator->ScratchTextures[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Barriers[NumBarriers], &NumBarriers);
				mz_AddTransitionBarrier(Texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &Barriers[NumBarriers], &NumBarriers);
				mz_CmdResourceBarrier(CmdList, NumBarriers, Barriers);
			}

			if ((TotalNumMipsToGen -= NumMipsInDispatch) == 0)
			{
				break;
			}
			CurrentSrcMipLevel += NumMipsInDispatch;
		}
	}
}

eastl::vector<uint8_t>
mz_LoadFile(const char* Name)
{
	FILE* File = fopen(Name, "rb");
	mz_ASSERT(File);
	fseek(File, 0, SEEK_END);
	long Size = ftell(File);
	if (Size <= 0)
	{
		mz_ASSERT(0);
		return eastl::vector<uint8_t>();
	}
	eastl::vector<uint8_t> Content(Size);
	fseek(File, 0, SEEK_SET);
	fread(Content.data(), 1, Content.size(), File);
	fclose(File);
	return Content;
}

void
mz_UpdateFrameStats(HWND Window, const char* Name, double* Time, float* DeltaTime)
{
	static double PreviousTime = -1.0;
	static double HeaderRefreshTime = 0.0;
	static uint32_t NumFrames = 0;

	if (PreviousTime < 0.0)
	{
		PreviousTime = mz_GetTime();
		HeaderRefreshTime = PreviousTime;
	}

	*Time = mz_GetTime();
	*DeltaTime = (float)(*Time - PreviousTime);
	PreviousTime = *Time;

	if ((*Time - HeaderRefreshTime) >= 1.0)
	{
		double FPS = NumFrames / (*Time - HeaderRefreshTime);
		double MS = (1.0 / FPS) * 1000.0;
		char Header[256];
		snprintf(Header, sizeof(Header), "[%.1f fps  %.3f ms] %s", FPS, MS, Name);
		SetWindowText(Window, Header);
		HeaderRefreshTime = *Time;
		NumFrames = 0;
	}
	NumFrames++;
}

double
mz_GetTime()
{
	static LARGE_INTEGER StartCounter;
	static LARGE_INTEGER Frequency;
	if (StartCounter.QuadPart == 0)
	{
		QueryPerformanceFrequency(&Frequency);
		QueryPerformanceCounter(&StartCounter);
	}
	LARGE_INTEGER Counter;
	QueryPerformanceCounter(&Counter);
	return (Counter.QuadPart - StartCounter.QuadPart) / (double)Frequency.QuadPart;
}

static LRESULT CALLBACK
mz_ProcessWindowMessage(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	ImGuiIO* IO = &ImGui::GetIO();

	switch (Message)
	{
	case WM_LBUTTONDOWN:
		IO->MouseDown[0] = true;
		return 0;
	case WM_LBUTTONUP:
		IO->MouseDown[0] = false;
		return 0;
	case WM_RBUTTONDOWN:
		IO->MouseDown[1] = true;
		return 0;
	case WM_RBUTTONUP:
		IO->MouseDown[1] = false;
		return 0;
	case WM_MBUTTONDOWN:
		IO->MouseDown[2] = true;
		return 0;
	case WM_MBUTTONUP:
		IO->MouseDown[2] = false;
		return 0;
	case WM_MOUSEWHEEL:
		IO->MouseWheel += GET_WHEEL_DELTA_WPARAM(WParam) > 0 ? 1.0f : -1.0f;
		return 0;
	case WM_MOUSEMOVE:
		IO->MousePos.x = (signed short)LParam;
		IO->MousePos.y = (signed short)(LParam >> 16);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_KEYDOWN:
	{
		if (WParam < 256)
		{
			IO->KeysDown[WParam] = true;
			if (WParam == VK_ESCAPE)
			{
				PostQuitMessage(0);
			}
			return 0;
		}
	}
	break;
	case WM_KEYUP:
	{
		if (WParam < 256)
		{
			IO->KeysDown[WParam] = false;
			return 0;
		}
	}
	break;
	case WM_CHAR:
	{
		if (WParam > 0 && WParam < 0x10000)
		{
			IO->AddInputCharacter((unsigned short)WParam);
			return 0;
		}
	}
	break;
	}
	return DefWindowProc(Window, Message, WParam, LParam);
}

HWND
mz_CreateWindow(const char* Name, uint32_t Width, uint32_t Height)
{
	WNDCLASS WinClass = {};
	WinClass.lpfnWndProc = mz_ProcessWindowMessage;
	WinClass.hInstance = GetModuleHandle(nullptr);
	WinClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
	WinClass.lpszClassName = Name;
	if (!RegisterClass(&WinClass))
	{
		mz_ASSERT(0);
	}

	RECT Rect = { 0, 0, (LONG)Width, (LONG)Height };
	if (!AdjustWindowRect(&Rect, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 0))
	{
		mz_ASSERT(0);
	}

	HWND Window = CreateWindowEx(0, Name, Name, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, Rect.right - Rect.left, Rect.bottom - Rect.top, nullptr, nullptr, nullptr, 0);
	mz_ASSERT(Window);
	return Window;
}

mz_DX12Resource*
mz_AddDX12Resource(mz_GraphicsContext* Gfx, ID3D12Resource* RawResource, D3D12_RESOURCE_STATES InitialState, DXGI_FORMAT Format)
{
	mz_DX12Resource* NewResource = nullptr;
	for (uint32_t Idx = 0; Idx < mz_MAX_NUM_RESOURCES; ++Idx)
	{
		if (Gfx->Resources[Idx].Raw == nullptr)
		{
			NewResource = &Gfx->Resources[Idx];
			NewResource->Raw = RawResource;
			NewResource->State = InitialState;
			NewResource->Format = Format;
			break;
		}
	}
	mz_ASSERT(NewResource);
	return NewResource;
}

mz_DX12Resource*
mz_CreateCommittedResource(mz_GraphicsContext* Gfx, D3D12_HEAP_TYPE HeapType, D3D12_HEAP_FLAGS HeapFlags, D3D12_RESOURCE_DESC* Desc, D3D12_RESOURCE_STATES InitialState, D3D12_CLEAR_VALUE* ClearValue)
{
	ID3D12Resource* RawResource;
	mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(HeapType), HeapFlags, Desc, InitialState, ClearValue, IID_PPV_ARGS(&RawResource)));

	return mz_AddDX12Resource(Gfx, RawResource, InitialState, Desc->Format);
}

mz_DX12PipelineState*
mz_CreateGraphicsPipelineState(mz_GraphicsContext* Gfx, D3D12_GRAPHICS_PIPELINE_STATE_DESC* PSODesc, const char* VSName, const char* PSName)
{
	char Path[MAX_PATH];

	snprintf(Path, sizeof(Path), "Data/Shaders/%s", VSName);
	eastl::vector<uint8_t> VSBytecode = mz_LoadFile(Path);

	snprintf(Path, sizeof(Path), "Data/Shaders/%s", PSName);
	eastl::vector<uint8_t> PSBytecode = mz_LoadFile(Path);

	uint64_t Hash;
	{
		meow_state HasherState;
		MeowBegin(&HasherState, MeowDefaultSeed);
		MeowAbsorb(&HasherState, VSBytecode.size(), VSBytecode.data());
		MeowAbsorb(&HasherState, PSBytecode.size(), PSBytecode.data());
		MeowAbsorb(&HasherState, sizeof(PSODesc->BlendState), &PSODesc->BlendState);
		MeowAbsorb(&HasherState, sizeof(PSODesc->SampleMask), &PSODesc->SampleMask);
		MeowAbsorb(&HasherState, sizeof(PSODesc->RasterizerState), &PSODesc->RasterizerState);
		MeowAbsorb(&HasherState, sizeof(PSODesc->DepthStencilState), &PSODesc->DepthStencilState);
		for (uint32_t Idx = 0; Idx < PSODesc->InputLayout.NumElements; ++Idx)
		{
			MeowAbsorb(&HasherState, sizeof(PSODesc->InputLayout.pInputElementDescs[0]), (void*)&PSODesc->InputLayout.pInputElementDescs[Idx]);
		}
		MeowAbsorb(&HasherState, sizeof(PSODesc->PrimitiveTopologyType), &PSODesc->PrimitiveTopologyType);
		MeowAbsorb(&HasherState, sizeof(PSODesc->NumRenderTargets), &PSODesc->NumRenderTargets);
		MeowAbsorb(&HasherState, sizeof(PSODesc->RTVFormats), &PSODesc->RTVFormats[0]);
		MeowAbsorb(&HasherState, sizeof(PSODesc->DSVFormat), &PSODesc->DSVFormat);
		MeowAbsorb(&HasherState, sizeof(PSODesc->SampleDesc), &PSODesc->SampleDesc);
		Hash = MeowU64From(MeowEnd(&HasherState, nullptr), 0);
	}

	auto Found = Gfx->GraphicsPipelinesMap.find(Hash);
	if (Found != Gfx->GraphicsPipelinesMap.end())
	{
		return Found->second;
	}

	mz_DX12PipelineState* NewPipeline = nullptr;
	for (uint32_t Idx = 0; Idx < mz_MAX_NUM_PIPELINES; ++Idx)
	{
		if (Gfx->Pipelines[Idx].PSO == nullptr && Gfx->Pipelines[Idx].RS == nullptr)
		{
			NewPipeline = &Gfx->Pipelines[Idx];
			break;
		}
	}
	mz_ASSERT(NewPipeline);

	for (auto Iter = Gfx->GraphicsPipelinesMap.begin(); Iter != Gfx->GraphicsPipelinesMap.end(); ++Iter)
	{
		if (Iter->second == NewPipeline)
		{
			Gfx->GraphicsPipelinesMap.erase(Iter);
			break;
		}
	}

	// NOTE(mziulek): Root signature has to be defined in HLSL code.
	mz_VHR(Gfx->Device->CreateRootSignature(0, VSBytecode.data(), VSBytecode.size(), IID_PPV_ARGS(&NewPipeline->RS)));

	PSODesc->pRootSignature = NewPipeline->RS;
	PSODesc->VS = { VSBytecode.data(), VSBytecode.size() };
	PSODesc->PS = { PSBytecode.data(), PSBytecode.size() };

	mz_VHR(Gfx->Device->CreateGraphicsPipelineState(PSODesc, IID_PPV_ARGS(&NewPipeline->PSO)));

	Gfx->GraphicsPipelinesMap.insert(eastl::make_pair(Hash, NewPipeline));

	return NewPipeline;
}

mz_DX12PipelineState*
mz_CreateComputePipelineState(mz_GraphicsContext* Gfx, D3D12_COMPUTE_PIPELINE_STATE_DESC* PSODesc, const char* CSName)
{
	char Path[MAX_PATH];

	snprintf(Path, sizeof(Path), "Data/Shaders/%s", CSName);
	eastl::vector<uint8_t> CSBytecode = mz_LoadFile(Path);

	uint64_t Hash = MeowU64From(MeowHash(MeowDefaultSeed, CSBytecode.size(), CSBytecode.data()), 0);

	auto Found = Gfx->ComputePipelinesMap.find(Hash);
	if (Found != Gfx->ComputePipelinesMap.end())
	{
		return Found->second;
	}

	mz_DX12PipelineState* NewPipeline = nullptr;
	for (uint32_t Idx = 0; Idx < mz_MAX_NUM_PIPELINES; ++Idx)
	{
		if (Gfx->Pipelines[Idx].PSO == nullptr && Gfx->Pipelines[Idx].RS == nullptr)
		{
			NewPipeline = &Gfx->Pipelines[Idx];
			break;
		}
	}
	mz_ASSERT(NewPipeline);

	for (auto Iter = Gfx->ComputePipelinesMap.begin(); Iter != Gfx->ComputePipelinesMap.end(); ++Iter)
	{
		if (Iter->second == NewPipeline)
		{
			Gfx->ComputePipelinesMap.erase(Iter);
			break;
		}
	}

	// NOTE(mziulek): Root signature has to be defined in HLSL code.
	mz_VHR(Gfx->Device->CreateRootSignature(0, CSBytecode.data(), CSBytecode.size(), IID_PPV_ARGS(&NewPipeline->RS)));

	PSODesc->pRootSignature = NewPipeline->RS;
	PSODesc->CS = { CSBytecode.data(), CSBytecode.size() };

	mz_VHR(Gfx->Device->CreateComputePipelineState(PSODesc, IID_PPV_ARGS(&NewPipeline->PSO)));

	Gfx->ComputePipelinesMap.insert(eastl::make_pair(Hash, NewPipeline));

	return NewPipeline;
}

static void
mz_LoadGLTFMesh(cgltf_mesh* InMesh, mz_Mesh* OutMesh, eastl::vector<mz_Vertex>* InOutVertices, eastl::vector<uint32_t>* InOutIndices)
{
	mz_ASSERT(InMesh);

	OutMesh->NumSections = (uint16_t)InMesh->primitives_count;
	if (OutMesh->NumSections > 1)
	{
		OutMesh->Sections = (mz_MeshSection*)calloc(OutMesh->NumSections, sizeof(mz_MeshSection));
		if (!OutMesh->Sections)
		{
			return;
		}
	}

	uint32_t TotalNumVertices = 0;
	uint32_t TotalNumIndices = 0;

	for (uint32_t SectionIdx = 0; SectionIdx < InMesh->primitives_count; ++SectionIdx)
	{
		mz_ASSERT(InMesh->primitives[SectionIdx].indices);
		mz_ASSERT(InMesh->primitives[SectionIdx].attributes);

		TotalNumIndices += (uint32_t)InMesh->primitives[SectionIdx].indices->count;
		TotalNumVertices += (uint32_t)InMesh->primitives[SectionIdx].attributes[0].data->count;
	}

	InOutVertices->reserve(InOutVertices->size() + TotalNumVertices);
	InOutIndices->reserve(InOutIndices->size() + TotalNumIndices);

	eastl::vector<XMFLOAT3> Positions;
	eastl::vector<XMFLOAT3> Normals;
	eastl::vector<XMFLOAT4> Tangents;
	eastl::vector<XMFLOAT2> Texcoords;
	Positions.reserve(TotalNumVertices);
	Normals.reserve(TotalNumVertices);
	Tangents.reserve(TotalNumVertices);
	Texcoords.reserve(TotalNumVertices);

	mz_MeshSection* Sections = mz_GetMeshSections(OutMesh);

	for (uint32_t SectionIdx = 0; SectionIdx < InMesh->primitives_count; ++SectionIdx)
	{
		// Indices.
		{
			cgltf_accessor* Accessor = InMesh->primitives[SectionIdx].indices;

			mz_ASSERT(Accessor->buffer_view);
			mz_ASSERT(Accessor->stride == Accessor->buffer_view->stride || Accessor->buffer_view->stride == 0);
			mz_ASSERT((Accessor->stride * Accessor->count) == Accessor->buffer_view->size);

			auto DataAddr = (uint8_t*)Accessor->buffer_view->buffer->data + Accessor->offset + Accessor->buffer_view->offset;

			Sections[SectionIdx].BaseIndex = (uint32_t)InOutIndices->size();
			Sections[SectionIdx].NumIndices = (uint32_t)Accessor->count;

			if (Accessor->stride == 1)
			{
				uint8_t* DataU8 = (uint8_t*)DataAddr;
				for (uint32_t Idx = 0; Idx < Accessor->count; ++Idx)
				{
					InOutIndices->push_back((uint32_t)*DataU8++);
				}
			}
			else if (Accessor->stride == 2)
			{
				uint16_t* DataU16 = (uint16_t*)DataAddr;
				for (uint32_t Idx = 0; Idx < Accessor->count; ++Idx)
				{
					InOutIndices->push_back((uint32_t)*DataU16++);
				}
			}
			else if (Accessor->stride == 4)
			{
				InOutIndices->resize(InOutIndices->size() + Accessor->count);

				memcpy(InOutIndices->data() + (InOutIndices->size() - Accessor->count), DataAddr, Accessor->count * Accessor->stride);
			}
			else
			{
				mz_ASSERT(0);
			}
		}

		// Attributes.
		{
			uint32_t NumAttribs = (uint32_t)InMesh->primitives[SectionIdx].attributes_count;

			for (uint32_t AttribIdx = 0; AttribIdx < NumAttribs; ++AttribIdx)
			{
				cgltf_attribute* Attrib = &InMesh->primitives[SectionIdx].attributes[AttribIdx];
				cgltf_accessor* Accessor = Attrib->data;

				mz_ASSERT(Accessor->buffer_view);
				mz_ASSERT(Accessor->stride == Accessor->buffer_view->stride || Accessor->buffer_view->stride == 0);
				mz_ASSERT((Accessor->stride * Accessor->count) == Accessor->buffer_view->size);

				auto DataAddr = (uint8_t*)Accessor->buffer_view->buffer->data + Accessor->offset + Accessor->buffer_view->offset;

				if (Attrib->type == cgltf_attribute_type_position)
				{
					mz_ASSERT(Accessor->type == cgltf_type_vec3);
					Positions.resize(Accessor->count);
					memcpy(Positions.data(), DataAddr, Accessor->count * Accessor->stride);
				}
				else if (Attrib->type == cgltf_attribute_type_normal)
				{
					mz_ASSERT(Accessor->type == cgltf_type_vec3);
					Normals.resize(Accessor->count);
					memcpy(Normals.data(), DataAddr, Accessor->count * Accessor->stride);
				}
				else if (Attrib->type == cgltf_attribute_type_tangent)
				{
					mz_ASSERT(Accessor->type == cgltf_type_vec4);
					Tangents.resize(Accessor->count);
					memcpy(Tangents.data(), DataAddr, Accessor->count * Accessor->stride);
				}
				else if (Attrib->type == cgltf_attribute_type_texcoord)
				{
					mz_ASSERT(Accessor->type == cgltf_type_vec2);
					Texcoords.resize(Accessor->count);
					memcpy(Texcoords.data(), DataAddr, Accessor->count * Accessor->stride);
				}
			}

			mz_ASSERT(Positions.size() > 0);
			mz_ASSERT(Positions.size() == Normals.size());
			mz_ASSERT(Positions.size() == Texcoords.size());

			if (Tangents.empty())
			{
				Tangents.resize(Positions.size());
			}

			Sections[SectionIdx].BaseVertex = (uint32_t)InOutVertices->size();
			Sections[SectionIdx].NumVertices = (uint32_t)Positions.size();

			for (uint32_t Idx = 0; Idx < Positions.size(); ++Idx)
			{
				mz_Vertex Vertex;
				Vertex.Position = Positions[Idx];
				Vertex.Normal = Normals[Idx];
				Vertex.Tangent = Tangents[Idx];
				Vertex.Texcoord = Texcoords[Idx];
				InOutVertices->push_back(Vertex);
			}

			Positions.clear();
			Normals.clear();
			Texcoords.clear();
			Tangents.clear();
		}
	}
}

void
mz_LoadGLTFScene(const char* FileName, mz_GraphicsContext* Gfx, mz_SceneData* OutScene, eastl::vector<ID3D12Resource*>* OutTempResources)
{
	mz_ASSERT(OutScene->Meshes.empty() && OutScene->Objects.empty() && OutScene->Materials.empty() && OutScene->Textures.empty() && OutScene->TextureSRVs.empty());

	cgltf_options Options = {};
	cgltf_data* Data = nullptr;
	{
		cgltf_result R = cgltf_parse_file(&Options, FileName, &Data);
		mz_ASSERT(R == cgltf_result_success);

		R = cgltf_load_buffers(&Options, Data, FileName);
		mz_ASSERT(R == cgltf_result_success);

		mz_ASSERT(Data->scenes_count == 1);
	}

	eastl::vector<mz_Vertex> AllVertices;
	eastl::vector<uint32_t> AllIndices;

	// Meshes.
	{
		uint32_t NumMeshes = (uint32_t)Data->meshes_count;
		mz_ASSERT(NumMeshes > 0);

		OutScene->Meshes.reserve(NumMeshes);

		for (uint32_t MeshIdx = 0; MeshIdx < NumMeshes; ++MeshIdx)
		{
			mz_Mesh Mesh = {};
			mz_LoadGLTFMesh(&Data->meshes[MeshIdx], &Mesh, &AllVertices, &AllIndices);

			cgltf_mesh* SrcMesh = &Data->meshes[MeshIdx];

			mz_MeshSection* Sections = mz_GetMeshSections(&Mesh);

			for (uint32_t SectionIdx = 0; SectionIdx < (uint32_t)SrcMesh->primitives_count; ++SectionIdx)
			{
				Sections[SectionIdx].MaterialIndex = (uint16_t)~0;

				for (uint32_t MaterialIdx = 0; MaterialIdx < (uint32_t)Data->materials_count; ++MaterialIdx)
				{
					if (&Data->materials[MaterialIdx] == SrcMesh->primitives[SectionIdx].material)
					{
						Sections[SectionIdx].MaterialIndex = (uint16_t)MaterialIdx;
						break;
					}
				}
				mz_ASSERT(Sections[SectionIdx].MaterialIndex != (uint16_t)~0);
			}

			OutScene->Meshes.push_back(Mesh);
		}
	}

	// Materials.
	{
		uint32_t NumMaterials = (uint32_t)Data->materials_count;
		mz_ASSERT(NumMaterials > 0);

		OutScene->Materials.reserve(NumMaterials);

		for (uint32_t MaterialIdx = 0; MaterialIdx < NumMaterials; ++MaterialIdx)
		{
			cgltf_material* SrcMaterial = &Data->materials[MaterialIdx];
			if (SrcMaterial->has_pbr_metallic_roughness)
			{
				cgltf_pbr_metallic_roughness* PBR = &SrcMaterial->pbr_metallic_roughness;

				mz_Material Material = {};
				Material.BaseColorFactor = XMFLOAT4(PBR->base_color_factor);
				Material.RoughnessFactor = PBR->roughness_factor;
				Material.MetallicFactor = PBR->metallic_factor;

				Material.BaseColorTextureIndex = (uint16_t)~0;
				Material.PBRFactorsTextureIndex = (uint16_t)~0;
				Material.NormalTextureIndex = (uint16_t)~0;

				for (uint32_t ImageIdx = 0; ImageIdx < (uint32_t)Data->images_count; ++ImageIdx)
				{
					cgltf_image* Image = &Data->images[ImageIdx];

					if (PBR->base_color_texture.texture && Image->uri == PBR->base_color_texture.texture->image->uri)
					{
						mz_ASSERT(Material.BaseColorTextureIndex == (uint16_t)~0);
						Material.BaseColorTextureIndex = (uint16_t)ImageIdx;
					}
					if (PBR->metallic_roughness_texture.texture && Image->uri == PBR->metallic_roughness_texture.texture->image->uri)
					{
						mz_ASSERT(Material.PBRFactorsTextureIndex == (uint16_t)~0);
						Material.PBRFactorsTextureIndex = (uint16_t)ImageIdx;
					}
					if (SrcMaterial->normal_texture.texture && Image->uri == SrcMaterial->normal_texture.texture->image->uri)
					{
						mz_ASSERT(Material.NormalTextureIndex == (uint16_t)~0);
						Material.NormalTextureIndex = (uint16_t)ImageIdx;
					}
				}

				OutScene->Materials.push_back(Material);
			}
		}
	}

	uint32_t NumNodes = (uint32_t)Data->nodes_count;
	mz_ASSERT(NumNodes > 0);

	OutScene->Objects.reserve(NumNodes);

	for (uint32_t NodeIdx = 0; NodeIdx < NumNodes; ++NodeIdx)
	{
		if (Data->nodes[NodeIdx].mesh)
		{
			cgltf_mesh* SrcMesh = Data->nodes[NodeIdx].mesh;

			mz_Object Object = {};
			Object.MeshIndex = (uint16_t)~0;

			for (uint32_t MeshIdx = 0; MeshIdx < (uint32_t)Data->meshes_count; ++MeshIdx)
			{
				if (&Data->meshes[MeshIdx] == SrcMesh)
				{
					Object.MeshIndex = (uint16_t)MeshIdx;
					break;
				}
			}
			mz_ASSERT(Object.MeshIndex != (uint16_t)~0);

			cgltf_float WorldTransform[16];
			cgltf_node_transform_world(&Data->nodes[NodeIdx], WorldTransform);

			memcpy(&Object.ObjectToWorld, &XMMatrixTranspose(XMLoadFloat4x4((XMFLOAT4X4*)&WorldTransform[0])), sizeof(XMFLOAT3X4));

			OutScene->Objects.push_back(Object);
		}
	}

	for (uint32_t ImageIdx = 0; ImageIdx < (uint32_t)Data->images_count; ++ImageIdx)
	{
		cgltf_image* Image = &Data->images[ImageIdx];

		char Path[MAX_PATH];
		strcpy(Path, FileName);

		char* C = strrchr(Path, '/');
		mz_ASSERT(C);
		*C = '\0';

		snprintf(Path, sizeof(Path), "%s/%s", Path, Image->uri);

		int Width, Height;
		uint8_t* ImageData = stbi_load(Path, &Width, &Height, nullptr, 4);
		mz_ASSERT(ImageData);

		mz_DX12Resource* Texture = nullptr;
		{
			auto Desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, (uint64_t)Width, (uint32_t)Height);
			Texture = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
			OutScene->Textures.push_back(Texture);

			D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
			Gfx->Device->CreateShaderResourceView(Texture->Raw, nullptr, CPUHandle);
			OutScene->TextureSRVs.push_back(CPUHandle);
		}

		{
			ID3D12Resource* UploadBuffer;
			auto Desc = CD3DX12_RESOURCE_DESC::Buffer(GetRequiredIntermediateSize(Texture->Raw, 0, 1));
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadBuffer)));
			OutTempResources->push_back(UploadBuffer);

			D3D12_SUBRESOURCE_DATA SrcData = {};
			SrcData.pData = ImageData;
			SrcData.RowPitch = 4 * Width;
			UpdateSubresources<1>(Gfx->CmdList, Texture->Raw, UploadBuffer, 0, 0, 1, &SrcData);
		}

		mz_CmdTransitionBarrier(Gfx->CmdList, Texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		stbi_image_free(ImageData);
	}

	cgltf_free(Data);


	// Static geometry vertex buffer (single buffer for all static meshes).
	{
		ID3D12Resource* TempVertexBuffer;
		D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(AllVertices.size() * sizeof(mz_Vertex));
		{
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempVertexBuffer)));
			OutTempResources->push_back(TempVertexBuffer);

			void* Addr;
			mz_VHR(TempVertexBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Addr));
			memcpy(Addr, AllVertices.data(), AllVertices.size() * sizeof(mz_Vertex));
			TempVertexBuffer->Unmap(0, nullptr);
		}

		OutScene->VertexBuffer = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

		Gfx->CmdList->CopyResource(OutScene->VertexBuffer->Raw, TempVertexBuffer);

		mz_CmdTransitionBarrier(Gfx->CmdList, OutScene->VertexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		OutScene->VertexBufferSRV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.NumElements = (uint32_t)AllVertices.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(mz_Vertex);
		Gfx->Device->CreateShaderResourceView(OutScene->VertexBuffer->Raw, &SRVDesc, OutScene->VertexBufferSRV);
	}

	// Static geometry index buffer (single buffer for all static meshes).
	{
		ID3D12Resource* TempIndexBuffer;
		D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(AllIndices.size() * sizeof(uint32_t));
		{
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempIndexBuffer)));
			OutTempResources->push_back(TempIndexBuffer);

			void* Addr;
			mz_VHR(TempIndexBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Addr));
			memcpy(Addr, AllIndices.data(), AllIndices.size() * sizeof(uint32_t));
			TempIndexBuffer->Unmap(0, nullptr);
		}

		OutScene->IndexBuffer = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

		Gfx->CmdList->CopyResource(OutScene->IndexBuffer->Raw, TempIndexBuffer);

		mz_CmdTransitionBarrier(Gfx->CmdList, OutScene->IndexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		OutScene->IndexBufferSRV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_R32G32B32_UINT;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.NumElements = (uint32_t)AllIndices.size() / 3;
		Gfx->Device->CreateShaderResourceView(OutScene->IndexBuffer->Raw, &SRVDesc, OutScene->IndexBufferSRV);
	}
}
