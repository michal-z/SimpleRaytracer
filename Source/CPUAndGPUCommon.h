#pragma once

#ifdef __cplusplus
#include "DirectXMath/DirectXMath.h"
using namespace DirectX;
typedef XMFLOAT4X4 float4x4;
typedef XMFLOAT4X3 float4x3;
typedef XMFLOAT2 float2;
typedef XMFLOAT3 float3;
typedef XMFLOAT4 float4;
typedef uint32_t uint;
#endif

#ifdef __cplusplus
#define SALIGN alignas(256)
#else
#define SALIGN
#endif

struct SALIGN mz_PerFrameConstantData
{
	float4x4 ProjectionToWorld;
	float4 CameraPosition;
	float4 LightPositions[1];
	float4 LightColors[1];
};

struct mz_Vertex
{
	float3 Position;
	float3 Normal;
	float4 Tangent;
	float2 Texcoord;
};

struct mz_Transform4x3
{
	float4x3 Transform;
};

struct mz_PerGeometryRootData
{
	uint ObjectIndex;
	uint BaseVertex;
	uint BaseIndex;
};

#ifdef __cplusplus
#undef SALIGN
#endif
