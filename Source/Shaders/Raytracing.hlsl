#include "../CPUAndGPUCommon.h"

#define PI 3.1415926f
#define MAX_RECURSION_DEPTH 3
#define FLT_MAX (asfloat(0x7F7FFFFF))

GlobalRootSignature GlobalSignature =
{
	"DescriptorTable(UAV(u0)),"
	"SRV(t0),"
	"CBV(b0),"
	"DescriptorTable(SRV(t1, numDescriptors = 3)),"
};

LocalRootSignature RadianceSignature =
{
	"RootConstants(num32BitConstants = 3, b1),"
	"DescriptorTable(SRV(t4, numDescriptors = 3)),"
	"StaticSampler(s0, filter = FILTER_ANISOTROPIC, maxAnisotropy = 16),"
};

SubobjectToExportsAssociation RadianceSignatureAssoc =
{
	"RadianceSignature", "RadianceHitGroup"
};

TriangleHitGroup RadianceHitGroup =
{
	"", "RadianceClosestHit"
};

TriangleHitGroup ShadowHitGroup =
{
	"", "ShadowClosestHit"
};

RaytracingShaderConfig ShaderConfig =
{
	16, // max payload size
	8, // max attribute size
};

RaytracingPipelineConfig PipelineConfig =
{
	MAX_RECURSION_DEPTH
};

RaytracingAccelerationStructure GScene : register(t0);
RWTexture2D<float4> GOutput : register(u0);
ConstantBuffer<mz_PerFrameConstantData> GPerFrameCB : register(b0);
StructuredBuffer<mz_Vertex> GVertexBuffer : register(t1);
Buffer<uint3> GIndexBuffer : register(t2);
StructuredBuffer<mz_Transform4x3> GObjectToWorld : register(t3);

ConstantBuffer<mz_PerGeometryRootData> GPerGeometryCB : register(b1);
Texture2D<float4> GBaseColorTexture : register(t4);
Texture2D<float4> GPBRFactorsTexture : register(t5);
Texture2D<float4> GNormalTexture : register(t6);
SamplerState GSampler : register(s0);


typedef BuiltInTriangleIntersectionAttributes FAttributes;
struct FPayload
{
	float3 Color;
	int CurrentRecursionDepth;
};

struct FShadowPayload
{
	float RayHitT;
};

void GenerateCameraRay(uint2 RayIndex, out float3 Origin, out float3 Direction)
{
	float2 XY = RayIndex + 0.5f;
	float2 ScreenPos = XY / DispatchRaysDimensions().xy * 2.0f - 1.0f;

	ScreenPos.y = -ScreenPos.y;

	float4 World = mul(float4(ScreenPos, 0.0f, 1.0f), GPerFrameCB.ProjectionToWorld);
	World.xyz /= World.w;

	Origin = GPerFrameCB.CameraPosition.xyz;
	Direction = normalize(World.xyz - Origin);
}

float3 TraceRadianceRay(float3 Origin, float3 Direction, float3 N, int RecursionDepth)
{
	if (RecursionDepth >= MAX_RECURSION_DEPTH)
	{
		return float3(0.0f, 0.0f, 0.0f);
	}

	RayDesc Ray;
	Ray.Origin = Origin + 0.001f * N;
	Ray.Direction = Direction;
	Ray.TMin = 0.0f;
	Ray.TMax = 100.0f;

	FPayload Payload;
	Payload.Color = float3(0.0f, 0.0f, 0.0f);
	Payload.CurrentRecursionDepth = RecursionDepth + 1;
	TraceRay(GScene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 1, 0, 2, 0, Ray, Payload);

	return Payload.Color;
}

bool TraceShadowRay(float3 Origin, float3 Direction, float3 N, int RecursionDepth)
{
	if (RecursionDepth >= MAX_RECURSION_DEPTH)
	{
		return false;
	}

	FShadowPayload Payload;
	Payload.RayHitT = FLT_MAX;

	RayDesc Ray;
	Ray.Origin = Origin + 0.001f * N;
	Ray.Direction = Direction;
	Ray.TMin = 0.0f;
	Ray.TMax = 100.0f;
	const uint Flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;

	TraceRay(GScene, Flags, 1, 1, 2, 1, Ray, Payload);

	bool bIsInShadow = Payload.RayHitT < FLT_MAX;

	return bIsInShadow;
}

[shader("raygeneration")]
void CameraRayGeneration()
{
	float3 Origin, Direction;
	GenerateCameraRay(DispatchRaysIndex().xy, Origin, Direction);

	float3 Color = TraceRadianceRay(Origin, Direction, 0.0f, 0);

	GOutput[DispatchRaysIndex().xy] = float4(Color, 1.0f);
}

[shader("miss")]
void RadianceMiss(inout FPayload Payload)
{
	Payload.Color = float3(0.1f, 0.2f, 0.4f);
}

[shader("miss")]
void ShadowMiss(inout FShadowPayload Payload)
{
	Payload.RayHitT = FLT_MAX;
}

float GeometrySchlickGGX(float CosTheta, float Roughness)
{
	float K = (Roughness * Roughness) * 0.5f;
	return CosTheta / (CosTheta * (1.0f - K) + K);
}

// Geometry function returns probability [0.0f, 1.0f].
float GeometrySmith(float NoL, float NoV, float Roughness)
{
	return saturate(GeometrySchlickGGX(NoV, Roughness) * GeometrySchlickGGX(NoL, Roughness));
}

// Trowbridge-Reitz GGX normal distribution function.
float DistributionGGX(float3 N, float3 H, float Roughness)
{
	float Alpha = Roughness * Roughness;
	float Alpha2 = Alpha * Alpha;
	float NoH = dot(N, H);
	float NoH2 = NoH * NoH;
	float K = NoH2 * Alpha2 + (1.0f - NoH2);
	return Alpha2 / (PI * K * K);
}

float3 FresnelSchlick(float CosTheta, float3 F0)
{
	return saturate(F0 + (1.0f - F0) * pow(1.0f - CosTheta, 5.0f));
}

[shader("closesthit")]
void ShadowClosestHit(inout FShadowPayload Payload, in FAttributes Attribs)
{
	Payload.RayHitT = RayTCurrent();
}

[shader("closesthit")]
void RadianceClosestHit(inout FPayload Payload, in FAttributes Attribs)
{
	float4x3 ObjectToWorld = GObjectToWorld[GPerGeometryCB.ObjectIndex].Transform;

	float3 N, PositionWS;
	float2 Texcoord;
	{
		uint3 Indices = GIndexBuffer[GPerGeometryCB.BaseIndex / 3 + PrimitiveIndex()] + GPerGeometryCB.BaseVertex;

		float3 Bary = float3(1.0f - Attribs.barycentrics.x - Attribs.barycentrics.y, Attribs.barycentrics.x, Attribs.barycentrics.y);

		float3 Positions[3] = { GVertexBuffer[Indices[0]].Position, GVertexBuffer[Indices[1]].Position, GVertexBuffer[Indices[2]].Position };
		float2 Texcoords[3] = { GVertexBuffer[Indices[0]].Texcoord, GVertexBuffer[Indices[1]].Texcoord, GVertexBuffer[Indices[2]].Texcoord };

		PositionWS = Positions[0] * Bary.x + Positions[1] * Bary.y + Positions[2] * Bary.z;
		Texcoord = Texcoords[0] * Bary.x + Texcoords[1] * Bary.y + Texcoords[2] * Bary.z;

		PositionWS = mul(float4(PositionWS, 1.0f), ObjectToWorld);

		float3 Normals[3] = { GVertexBuffer[Indices[0]].Normal, GVertexBuffer[Indices[1]].Normal, GVertexBuffer[Indices[2]].Normal };
		float3 Tangents[3] = { GVertexBuffer[Indices[0]].Tangent.xyz, GVertexBuffer[Indices[1]].Tangent.xyz, GVertexBuffer[Indices[2]].Tangent.xyz };

		float3 Normal = normalize(Normals[0] * Bary.x + Normals[1] * Bary.y + Normals[2] * Bary.z);
		float3 Tangent = normalize(Tangents[0] * Bary.x + Tangents[1] * Bary.y + Tangents[2] * Bary.z);
		float3 Bitangent = normalize(cross(Normal, Tangent)) * GVertexBuffer[Indices[0]].Tangent.w;

		N = normalize(GNormalTexture.SampleLevel(GSampler, Texcoord, 0).rgb * 2.0f - 1.0f);

		float3x3 TBN = float3x3(Tangent, Bitangent, Normal);
		N = mul(N, TBN);
		N = normalize(mul(N, (float3x3)ObjectToWorld));
	}
	float3 V = normalize(GPerFrameCB.CameraPosition.xyz - PositionWS);
	float NoV = saturate(dot(N, V));

	float3 LightVector = GPerFrameCB.LightPositions[0].xyz - PositionWS;
	float3 L = normalize(LightVector);

	bool bIsInShadow = TraceShadowRay(PositionWS, L, N, Payload.CurrentRecursionDepth);

	float3 Albedo = pow(GBaseColorTexture.SampleLevel(GSampler, Texcoord, 0).rgb, 2.2f);
	float3 PBRFactors = GPBRFactorsTexture.SampleLevel(GSampler, Texcoord, 0).rgb;
	float Roughness = PBRFactors.g;
	float Metallic = PBRFactors.b;
	float AO = PBRFactors.r;
	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), Albedo, Metallic);

	float3 Lo = 0.0f;

	// Light contribution.
	{
		float3 H = normalize(L + V);
		float NoL = saturate(dot(N, L));
		float HoV = saturate(dot(H, V));

		float Attenuation = max(1.0f / dot(LightVector, LightVector), 0.001f);
		float3 Radiance = GPerFrameCB.LightColors[0].rgb * Attenuation;

		float3 F = FresnelSchlick(HoV, F0);
		float ND = DistributionGGX(N, H, Roughness);
		float G = GeometrySmith(NoL, NoV, (Roughness + 1.0f) * 0.5f); // Note that 'Roughness' is remapped to '(Roughness + 1.0f) / 2.0f'.

		float3 Specular = (ND * G * F) / max(4.0f * NoV * NoL, 0.001f);

		float3 KD = (1.0f - F) * (1.0f - Metallic);

		Lo += (KD * (Albedo / PI) + Specular) * Radiance * NoL;
	}

	float3 Ambient = 0.03f * Albedo * AO;
	float3 Color = Ambient + Lo;

	Color = Color * (bIsInShadow ? 0.05f : 1.0f);

	Color = Color / (Color + 1.0f);
	Color = pow(Color, 1.0f / 2.2f);

	Payload.Color = Color;
}
