#include "Library.h"
#include <stdio.h>
#include "CPUAndGPUCommon.h"
#include "imgui/imgui.h"
#include "DirectXMath/DirectXPackedVector.h"
using namespace DirectX::PackedVector;

#define mz_DEMO_NAME "SimpleRaytracer"

struct mz_DemoRoot
{
	mz_GraphicsContext* Gfx;
	mz_UIContext* UI;
	ID3D12StateObject* RTPipeline;
	ID3D12RootSignature* RTGlobalSignature;
	mz_DX12Resource* BLASBuffer;
	mz_DX12Resource* TLASBuffer;
	mz_DX12Resource* ShaderTables[2];
	mz_DX12Resource* UploadShaderTables[2];
	mz_DX12Resource* RTOutput;
	D3D12_CPU_DESCRIPTOR_HANDLE RTOutputUAV;
	XMFLOAT3 CameraPosition;
	float CameraRotation[2];
	XMFLOAT3 LightPosition;
	mz_SceneData Scene;
	mz_DX12Resource* ObjectTransforms;
	D3D12_CPU_DESCRIPTOR_HANDLE ObjectTransformsSRV;
};

static void
mz_Update(mz_DemoRoot* Root)
{
	double Time;
	float DeltaTime;
	mz_UpdateFrameStats(Root->Gfx->Window, mz_DEMO_NAME, &Time, &DeltaTime);
	mz_UpdateUI(DeltaTime);

	{
		ImGuiIO* IO = &ImGui::GetIO();
		if (IO->MouseDown[1])
		{
			Root->CameraRotation[0] += IO->MouseDelta.x * 0.005f;
			Root->CameraRotation[1] += IO->MouseDelta.y * 0.005f;
		}

		XMVECTOR Position = XMLoadFloat3(&Root->CameraPosition);
		XMVECTOR Forward = XMVector3Transform(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), XMMatrixRotationRollPitchYaw(Root->CameraRotation[1], Root->CameraRotation[0], 0.0f));
		XMVECTOR Right = XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), Forward);

		float MoveScale = DeltaTime;

		if (GetAsyncKeyState('W') & 0x8000)
		{
			Position += MoveScale * Forward;
		}
		else if (GetAsyncKeyState('S') & 0x8000)
		{
			Position -= MoveScale * Forward;
		}

		if (GetAsyncKeyState('D') & 0x8000)
		{
			Position += MoveScale * Right;
		}
		else if (GetAsyncKeyState('A') & 0x8000)
		{
			Position -= MoveScale * Right;
		}

		XMStoreFloat3(&Root->CameraPosition, Position);
	}

	ImGui::ShowDemoWindow();
}

static void
mz_Draw(mz_DemoRoot* Root)
{
	mz_GraphicsContext* Gfx = Root->Gfx;
	ID3D12GraphicsCommandList5* CmdList = mz_CmdInit(Gfx);

	mz_DX12Resource* BackBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV;
	mz_GetBackBuffer(Gfx, &BackBuffer, &BackBufferRTV);

	// Update shader table.
	uint32_t NumRecordsInHitGroup = 0;
	uint32_t HitGroupRecordSize = 64;
	{
		uint8_t* ShaderTableAddr;
		mz_VHR(Root->UploadShaderTables[Gfx->FrameIndex]->Raw->Map(0, &CD3DX12_RANGE(0, 0), (void**)&ShaderTableAddr));

		ID3D12StateObject* Pipeline = Root->RTPipeline;
		ID3D12StateObjectProperties* Props;
		mz_VHR(Pipeline->QueryInterface(IID_PPV_ARGS(&Props)));

		memcpy(ShaderTableAddr + 0, Props->GetShaderIdentifier(L"CameraRayGeneration"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		memcpy(ShaderTableAddr + 64, Props->GetShaderIdentifier(L"RadianceMiss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
		memcpy(ShaderTableAddr + 64 + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, Props->GetShaderIdentifier(L"ShadowMiss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

		void* RadianceHitGroupID = Props->GetShaderIdentifier(L"RadianceHitGroup");
		void* ShadowHitGroupID = Props->GetShaderIdentifier(L"ShadowHitGroup");

		uint8_t* Base = ShaderTableAddr + 128;

		mz_SceneData* Scene = &Root->Scene;

		for (uint32_t ObjectIdx = 0; ObjectIdx < Scene->Objects.size(); ++ObjectIdx)
		{
			mz_Object* Object = &Scene->Objects[ObjectIdx];
			mz_Mesh* Mesh = &Scene->Meshes[Object->MeshIndex];

			mz_MeshSection* Sections = mz_GetMeshSections(Mesh);

			for (uint32_t SectionIdx = 0; SectionIdx < Mesh->NumSections; ++SectionIdx)
			{
				mz_ASSERT(Sections[SectionIdx].MaterialIndex != (uint16_t)~0);

				uint32_t MaterialIdx = (uint32_t)Sections[SectionIdx].MaterialIndex;
				mz_Material* Material = &Scene->Materials[MaterialIdx];

				// Shader Record 0 (RadianceHitGroup).
				memcpy(Base, RadianceHitGroupID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

				auto GeometryCB = (mz_PerGeometryRootData*)(Base + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

				GeometryCB->ObjectIndex = ObjectIdx;
				GeometryCB->BaseVertex = Sections[SectionIdx].BaseVertex;
				GeometryCB->BaseIndex = Sections[SectionIdx].BaseIndex;

				D3D12_GPU_DESCRIPTOR_HANDLE TableBase = mz_CopyDescriptorsToGPUHeap(Gfx, 1, Scene->TextureSRVs[Material->BaseColorTextureIndex]);
				if (Material->PBRFactorsTextureIndex != (uint16_t)~0)
				{
					mz_CopyDescriptorsToGPUHeap(Gfx, 1, Scene->TextureSRVs[Material->PBRFactorsTextureIndex]);
				}
				else
				{
					mz_CopyDescriptorsToGPUHeap(Gfx, 1, Scene->TextureSRVs[Material->BaseColorTextureIndex]);
				}

				if (Material->NormalTextureIndex != (uint16_t)~0)
				{
					mz_CopyDescriptorsToGPUHeap(Gfx, 1, Scene->TextureSRVs[Material->NormalTextureIndex]);
				}
				else
				{
					mz_CopyDescriptorsToGPUHeap(Gfx, 1, Scene->TextureSRVs[Material->BaseColorTextureIndex]);
				}

				*(D3D12_GPU_DESCRIPTOR_HANDLE*)(Base + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + 16) = TableBase;
				Base += HitGroupRecordSize;


				// Shader Record 1 (ShadowHitGroup).
				memcpy(Base, ShadowHitGroupID, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
				Base += HitGroupRecordSize;

				NumRecordsInHitGroup += 2;
			}
		}

		mz_SAFE_RELEASE(Props);
		Root->UploadShaderTables[Gfx->FrameIndex]->Raw->Unmap(0, nullptr);


		mz_CmdTransitionBarrier(Gfx->CmdList, Root->ShaderTables[Gfx->FrameIndex], D3D12_RESOURCE_STATE_COPY_DEST);

		Gfx->CmdList->CopyResource(Root->ShaderTables[Gfx->FrameIndex]->Raw, Root->UploadShaderTables[Gfx->FrameIndex]->Raw);

		mz_CmdTransitionBarrier(Gfx->CmdList, Root->ShaderTables[Gfx->FrameIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// Raytrace and copy result to the back buffer.
	{
		XMVECTOR Forward = XMVector3Transform(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), XMMatrixRotationRollPitchYaw(Root->CameraRotation[1], Root->CameraRotation[0], 0.0f));
		XMMATRIX ViewTransform = XMMatrixLookToLH(XMLoadFloat3(&Root->CameraPosition), Forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		XMMATRIX ProjectionTransform = XMMatrixPerspectiveFovLH(XM_PI / 3, (float)Gfx->Resolution[0] / Gfx->Resolution[1], 0.1f, 100.0f);
		XMMATRIX ProjectionToWorld = XMMatrixTranspose(XMMatrixInverse(nullptr, ViewTransform * ProjectionTransform));

		D3D12_GPU_VIRTUAL_ADDRESS GPUAddress;
		auto* CPUAddress = (mz_PerFrameConstantData*)mz_AllocateGPUMemory(Gfx, sizeof(mz_PerFrameConstantData), &GPUAddress);
		XMStoreFloat4x4(&CPUAddress->ProjectionToWorld, ProjectionToWorld);
		{
			XMFLOAT3 P = Root->CameraPosition;
			CPUAddress->CameraPosition = XMFLOAT4(P.x, P.y, P.z, 1.0f);
		}
		{
			XMFLOAT3 P = Root->LightPosition;
			CPUAddress->LightPositions[0] = XMFLOAT4(P.x, P.y, P.z, 1.0f);
		}
		CPUAddress->LightColors[0] = XMFLOAT4(600.0f, 600.0f, 400.0f, 1.0f);

		CmdList->SetPipelineState1(Root->RTPipeline);
		CmdList->SetComputeRootSignature(Root->RTGlobalSignature);
		CmdList->SetComputeRootDescriptorTable(0, mz_CopyDescriptorsToGPUHeap(Gfx, 1, Root->RTOutputUAV));
		CmdList->SetComputeRootShaderResourceView(1, Root->TLASBuffer->Raw->GetGPUVirtualAddress());
		CmdList->SetComputeRootConstantBufferView(2, GPUAddress);
		{
			D3D12_GPU_DESCRIPTOR_HANDLE TableBase = mz_CopyDescriptorsToGPUHeap(Gfx, 1, Root->Scene.VertexBufferSRV);
			mz_CopyDescriptorsToGPUHeap(Gfx, 1, Root->Scene.IndexBufferSRV);
			mz_CopyDescriptorsToGPUHeap(Gfx, 1, Root->ObjectTransformsSRV);
			CmdList->SetComputeRootDescriptorTable(3, TableBase);
		}

		{
			D3D12_GPU_VIRTUAL_ADDRESS Base = Root->ShaderTables[Gfx->FrameIndex]->Raw->GetGPUVirtualAddress();
			D3D12_DISPATCH_RAYS_DESC DispatchDesc = {};
			DispatchDesc.RayGenerationShaderRecord = { Base, 32 };
			DispatchDesc.MissShaderTable = { Base + 64, 64, 32 };
			DispatchDesc.HitGroupTable = { Base + 128, NumRecordsInHitGroup * HitGroupRecordSize, HitGroupRecordSize };
			DispatchDesc.Width = Gfx->Resolution[0];
			DispatchDesc.Height = Gfx->Resolution[1];
			DispatchDesc.Depth = 1;
			CmdList->DispatchRays(&DispatchDesc);
		}

		{
			uint32_t NumBarriers = 0;
			CD3DX12_RESOURCE_BARRIER Barriers[2];
			mz_AddTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, &Barriers[NumBarriers], &NumBarriers);
			mz_AddTransitionBarrier(Root->RTOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, &Barriers[NumBarriers], &NumBarriers);
			mz_CmdResourceBarrier(CmdList, NumBarriers, Barriers);
		}

		CmdList->CopyResource(BackBuffer->Raw, Root->RTOutput->Raw);

		{
			uint32_t NumBarriers = 0;
			CD3DX12_RESOURCE_BARRIER Barriers[2];
			mz_AddTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, &Barriers[NumBarriers], &NumBarriers);
			mz_AddTransitionBarrier(Root->RTOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &Barriers[NumBarriers], &NumBarriers);
			mz_CmdResourceBarrier(CmdList, NumBarriers, Barriers);
		}
	}

	// Draw UI.
	{
		CmdList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, (float)Gfx->Resolution[0], (float)Gfx->Resolution[1]));
		CmdList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, (LONG)Gfx->Resolution[0], (LONG)Gfx->Resolution[1]));

		CmdList->OMSetRenderTargets(1, &BackBufferRTV, TRUE, nullptr);

		mz_DrawUI(Root->UI, Gfx);

		mz_CmdTransitionBarrier(CmdList, BackBuffer, D3D12_RESOURCE_STATE_PRESENT);
	}

	CmdList->Close();

	Gfx->CmdQueue->ExecuteCommandLists(1, CommandListCast(&CmdList));
}

static void
mz_CreateBLAS(mz_SceneData* Scene, mz_GraphicsContext* Gfx, mz_DX12Resource** OutBLASBuffer, eastl::vector<ID3D12Resource*>* OutTempResources)
{
	D3D12_RAYTRACING_GEOMETRY_DESC GeometryDescTemplate = {};
	GeometryDescTemplate.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	GeometryDescTemplate.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	GeometryDescTemplate.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	GeometryDescTemplate.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
	GeometryDescTemplate.Triangles.VertexBuffer.StrideInBytes = (uint32_t)sizeof(mz_Vertex);

	eastl::vector<D3D12_RAYTRACING_GEOMETRY_DESC> GeometryDescs;
	eastl::vector<XMFLOAT3X4> Transforms;

	for (uint32_t ObjectIdx = 0; ObjectIdx < Scene->Objects.size(); ++ObjectIdx)
	{
		mz_Object* Object = &Scene->Objects[ObjectIdx];
		mz_Mesh* Mesh = &Scene->Meshes[Object->MeshIndex];

		mz_MeshSection* Sections = mz_GetMeshSections(Mesh);

		for (uint32_t SectionIdx = 0; SectionIdx < Mesh->NumSections; ++SectionIdx)
		{
			GeometryDescs.push_back(GeometryDescTemplate);
			D3D12_RAYTRACING_GEOMETRY_DESC* GeometryDesc = &GeometryDescs.back();
			GeometryDesc->Triangles.VertexBuffer.StartAddress = Scene->VertexBuffer->Raw->GetGPUVirtualAddress() + Sections[SectionIdx].BaseVertex * sizeof(mz_Vertex);
			GeometryDesc->Triangles.VertexCount = Sections[SectionIdx].NumVertices;
			GeometryDesc->Triangles.IndexBuffer = Scene->IndexBuffer->Raw->GetGPUVirtualAddress() + Sections[SectionIdx].BaseIndex * sizeof(uint32_t);
			GeometryDesc->Triangles.IndexCount = Sections[SectionIdx].NumIndices;
			Transforms.emplace_back(XMFLOAT3X4{});
			memcpy(&Transforms.back(), &Object->ObjectToWorld, sizeof(XMFLOAT3X4));
		}
	}

	ID3D12Resource* TransformBuffer;
	{
		ID3D12Resource* TempTransformBuffer;
		{
			CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(Transforms.size() * sizeof(Transforms[0]));
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempTransformBuffer)));
			OutTempResources->push_back(TempTransformBuffer);

			void* Addr;
			mz_VHR(TempTransformBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Addr));
			memcpy(Addr, Transforms.data(), Transforms.size() * sizeof(Transforms[0]));
			TempTransformBuffer->Unmap(0, nullptr);
		}

		// Create TransformBuffer.
		{
			CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(Transforms.size() * sizeof(Transforms[0]));
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&TransformBuffer)));
			OutTempResources->push_back(TransformBuffer);

			Gfx->CmdList->CopyResource(TransformBuffer, TempTransformBuffer);
			Gfx->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(TransformBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
	}

	for (uint32_t GeometryIdx = 0; GeometryIdx < GeometryDescs.size(); ++GeometryIdx)
	{
		GeometryDescs[GeometryIdx].Triangles.Transform3x4 = TransformBuffer->GetGPUVirtualAddress() + GeometryIdx * sizeof(XMFLOAT3X4);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS BLASInputs = {};
	BLASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	BLASInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	BLASInputs.NumDescs = (uint32_t)GeometryDescs.size();
	BLASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	BLASInputs.pGeometryDescs = GeometryDescs.data();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO BLASBuildInfo = {};
	Gfx->Device->GetRaytracingAccelerationStructurePrebuildInfo(&BLASInputs, &BLASBuildInfo);

	ID3D12Resource* ScratchBuffer;
	{
		CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(BLASBuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&ScratchBuffer)));
		OutTempResources->push_back(ScratchBuffer);
	}

	// Create BLAS result buffer.
	{
		CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(BLASBuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		*OutBLASBuffer = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BLASBuildDesc = {};
	BLASBuildDesc.Inputs = BLASInputs;
	BLASBuildDesc.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress();
	BLASBuildDesc.DestAccelerationStructureData = (*OutBLASBuffer)->Raw->GetGPUVirtualAddress();

	Gfx->CmdList->BuildRaytracingAccelerationStructure(&BLASBuildDesc, 0, nullptr);
	Gfx->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV((*OutBLASBuffer)->Raw));
}

static void
mz_CreateTLAS(mz_DX12Resource* BLASBuffer, mz_GraphicsContext* Gfx, mz_DX12Resource** OutTLASBuffer, eastl::vector<ID3D12Resource*>* OutTempResources)
{
	ID3D12Resource* InstanceBuffer;
	{
		D3D12_RAYTRACING_INSTANCE_DESC InstanceDesc = {};
		InstanceDesc.InstanceMask = 1;
		InstanceDesc.AccelerationStructure = BLASBuffer->Raw->GetGPUVirtualAddress();
		memcpy(&InstanceDesc.Transform, &XMMatrixTranspose(XMMatrixIdentity()), sizeof(InstanceDesc.Transform));

		ID3D12Resource* TempInstanceBuffer;
		{
			CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InstanceDesc));
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempInstanceBuffer)));
			OutTempResources->push_back(TempInstanceBuffer);

			void* Addr;
			mz_VHR(TempInstanceBuffer->Map(0, &CD3DX12_RANGE(0, 0), &Addr));
			memcpy(Addr, &InstanceDesc, sizeof(InstanceDesc));
			TempInstanceBuffer->Unmap(0, nullptr);
		}

		// Create InstanceBuffer.
		{
			CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InstanceDesc));
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&InstanceBuffer)));
			OutTempResources->push_back(InstanceBuffer);

			Gfx->CmdList->CopyResource(InstanceBuffer, TempInstanceBuffer);
			Gfx->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(InstanceBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TLASInputs = {};
	TLASInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	TLASInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	TLASInputs.NumDescs = 1;
	TLASInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	TLASInputs.InstanceDescs = InstanceBuffer->GetGPUVirtualAddress();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TLASBuildInfo = {};
	Gfx->Device->GetRaytracingAccelerationStructurePrebuildInfo(&TLASInputs, &TLASBuildInfo);

	ID3D12Resource* ScratchBuffer;
	{
		auto Desc = CD3DX12_RESOURCE_DESC::Buffer(TLASBuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&ScratchBuffer)));
		OutTempResources->push_back(ScratchBuffer);
	}

	// Create TLASBuffer.
	{
		auto Desc = CD3DX12_RESOURCE_DESC::Buffer(TLASBuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		*OutTLASBuffer = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr);
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC TLASBuildDesc = {};
	TLASBuildDesc.Inputs = TLASInputs;
	TLASBuildDesc.ScratchAccelerationStructureData = ScratchBuffer->GetGPUVirtualAddress();
	TLASBuildDesc.DestAccelerationStructureData = (*OutTLASBuffer)->Raw->GetGPUVirtualAddress();

	Gfx->CmdList->BuildRaytracingAccelerationStructure(&TLASBuildDesc, 0, nullptr);
	Gfx->CmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV((*OutTLASBuffer)->Raw));
}

static void
mz_CreateStaticGeometry(mz_DemoRoot* Root, eastl::vector<ID3D12Resource*>* OutTempResources, eastl::vector<mz_DX12Resource*>* OutTexturesThatNeedMipmaps)
{
	mz_GraphicsContext* Gfx = Root->Gfx;

	mz_LoadGLTFScene("Data/Sponza/Sponza.gltf", Gfx, &Root->Scene, OutTempResources);

	for (uint32_t Idx = 0; Idx < Root->Scene.Textures.size(); ++Idx)
	{
		OutTexturesThatNeedMipmaps->push_back(Root->Scene.Textures[Idx]);
	}

	// ObjectToWorld transformation matrix for each object in the world.
	{
		mz_SceneData* Scene = &Root->Scene;
		ID3D12Resource* TempBuffer;
		D3D12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Buffer(Scene->Objects.size() * sizeof(mz_Transform4x3));
		{
			mz_VHR(Gfx->Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempBuffer)));
			OutTempResources->push_back(TempBuffer);

			mz_Transform4x3* Addr;
			mz_VHR(TempBuffer->Map(0, &CD3DX12_RANGE(0, 0), (void**)&Addr));
			for (uint32_t ObjectIdx = 0; ObjectIdx < Scene->Objects.size(); ++ObjectIdx)
			{
				memcpy(&Addr[ObjectIdx], &Scene->Objects[ObjectIdx].ObjectToWorld, sizeof(*Addr));
			}
			TempBuffer->Unmap(0, nullptr);
		}

		Root->ObjectTransforms = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);

		Gfx->CmdList->CopyResource(Root->ObjectTransforms->Raw, TempBuffer);

		mz_CmdTransitionBarrier(Gfx->CmdList, Root->ObjectTransforms, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		Root->ObjectTransformsSRV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Buffer.NumElements = (uint32_t)Scene->Objects.size();
		SRVDesc.Buffer.StructureByteStride = sizeof(mz_Transform4x3);
		Gfx->Device->CreateShaderResourceView(Root->ObjectTransforms->Raw, &SRVDesc, Root->ObjectTransformsSRV);
	}

	mz_CreateBLAS(&Root->Scene, Gfx, &Root->BLASBuffer, OutTempResources);
	mz_CreateTLAS(Root->BLASBuffer, Gfx, &Root->TLASBuffer, OutTempResources);
}

static bool
mz_Init(mz_DemoRoot* Root)
{
	mz_GraphicsContext* Gfx = Root->Gfx;

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5 = {};
		Gfx->Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &Options5, sizeof(Options5));
		if (Options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
		{
			MessageBox(Gfx->Window, "This application requires GPU with raytracing support.", "Raytracing is not supported", MB_OK | MB_ICONERROR);
			return false;
		}
	}

	eastl::vector<ID3D12Resource*> TempResources;
	eastl::vector<mz_DX12Resource*> TexturesThatNeedMipmaps;

	Root->UI = mz_CreateUIContext(Gfx, 1, &TempResources);

	// Raytracing pipeline.
	{
		char Path[MAX_PATH];
		snprintf(Path, sizeof(Path), "Data/Shaders/%s", "Raytracing.lib.cso");
		eastl::vector<uint8_t> DXIL = mz_LoadFile(Path);

		D3D12_DXIL_LIBRARY_DESC LibraryDesc = {};
		LibraryDesc.DXILLibrary = CD3DX12_SHADER_BYTECODE(DXIL.data(), DXIL.size());

		D3D12_STATE_SUBOBJECT Subobject = {};
		Subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		Subobject.pDesc = &LibraryDesc;

		D3D12_STATE_OBJECT_DESC Desc = {};
		Desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		Desc.NumSubobjects = 1;
		Desc.pSubobjects = &Subobject;

		mz_VHR(Gfx->Device->CreateStateObject(&Desc, IID_PPV_ARGS(&Root->RTPipeline)));
		mz_VHR(Gfx->Device->CreateRootSignature(0, DXIL.data(), DXIL.size(), IID_PPV_ARGS(&Root->RTGlobalSignature)));
	}

	mz_CreateStaticGeometry(Root, &TempResources, &TexturesThatNeedMipmaps);

	// Create Shader Table.
	{
		auto Desc = CD3DX12_RESOURCE_DESC::Buffer(64 * 1024);

		for (uint32_t Idx = 0; Idx < eastl::size(Root->UploadShaderTables); ++Idx)
		{
			Root->UploadShaderTables[Idx] = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);
			Root->ShaderTables[Idx] = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
		}
	}

	// Create output texture for raytracing stage.
	{
		auto Desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, Gfx->Resolution[0], Gfx->Resolution[1], 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		Root->RTOutput = mz_CreateCommittedResource(Gfx, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
		Root->RTOutputUAV = mz_AllocateDescriptors(Gfx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

		Gfx->Device->CreateUnorderedAccessView(Root->RTOutput->Raw, nullptr, nullptr, Root->RTOutputUAV);
	}

	// Execute "data upload" and "data generation" GPU commands, create mipmaps etc. Destroy temp resources when GPU is done.
	{
		DXGI_FORMAT Formats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM };
		mz_MipmapGenerator* MipmapGenerators[eastl::size(Formats)] = {};
		for (uint32_t Idx = 0; Idx < eastl::size(Formats); ++Idx)
		{
			MipmapGenerators[Idx] = mz_CreateMipmapGenerator(Root->Gfx, Formats[Idx]);
		}

		for (mz_DX12Resource* Texture : TexturesThatNeedMipmaps)
		{
			D3D12_RESOURCE_DESC Desc = Texture->Raw->GetDesc();

			for (uint32_t Idx = 0; Idx < eastl::size(Formats); ++Idx)
			{
				if (Desc.Format == Formats[Idx])
				{
					mz_GenerateMipmaps(MipmapGenerators[Idx], Root->Gfx, Texture);
					break;
				}
			}
		}

		Root->Gfx->CmdList->Close();
		Root->Gfx->CmdQueue->ExecuteCommandLists(1, CommandListCast(&Root->Gfx->CmdList));
		mz_WaitForGPU(Root->Gfx);

		for (ID3D12Resource* Resource : TempResources)
		{
			mz_SAFE_RELEASE(Resource);
		}
		for (uint32_t Idx = 0; Idx < eastl::size(MipmapGenerators); ++Idx)
		{
			mz_DestroyMipmapGenerator(MipmapGenerators[Idx]);
		}
	}

	Root->CameraPosition = XMFLOAT3(0.0f, 0.5f, 0.0f);
	Root->LightPosition = XMFLOAT3(0.0f, 10.0f, 0.0f);

	return true;
}

static void
mz_Shutdown(mz_DemoRoot* Root)
{
	mz_SAFE_RELEASE(Root->RTPipeline);
	mz_SAFE_RELEASE(Root->RTGlobalSignature);
	for (uint32_t Idx = 0; Idx < Root->Scene.Meshes.size(); ++Idx)
	{
		mz_DestroyMesh(&Root->Scene.Meshes[Idx]);
	}
	mz_DestroyUIContext(Root->UI);
}

static int32_t
mz_Run(mz_DemoRoot* Root)
{
	ImGui::CreateContext();

	HWND Window = mz_CreateWindow(mz_DEMO_NAME, 1920, 1080);
	Root->Gfx = mz_CreateGraphicsContext(Window, /*bShouldCreateDepthBuffer*/false);

	if (mz_Init(Root))
	{
		for (;;)
		{
			MSG Message = {};
			if (PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
			{
				DispatchMessage(&Message);
				if (Message.message == WM_QUIT)
				{
					break;
				}
			}
			else
			{
				mz_Update(Root);
				mz_Draw(Root);
				mz_PresentFrame(Root->Gfx, 0);
			}
		}
	}

	mz_WaitForGPU(Root->Gfx);
	mz_Shutdown(Root);
	mz_DestroyGraphicsContext(Root->Gfx);
	ImGui::DestroyContext();

	return 0;
}

int32_t CALLBACK
WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int32_t)
{
	SetProcessDPIAware();
	mz_DemoRoot Root = {};
	return mz_Run(&Root);
}
