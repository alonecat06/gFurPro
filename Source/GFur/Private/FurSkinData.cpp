// Copyright 2023 GiM s.r.o. All Rights Reserved.

#include "FurSkinData.h"
#include "Runtime/Engine/Public/Rendering/SkeletalMeshRenderData.h"
#include "Runtime/Engine/Private/SkeletalRenderGPUSkin.h"
#include "Runtime/Renderer/Public/MeshMaterialShader.h"
#include "Runtime/RHI/Public/RHICommandList.h"
#include "ShaderParameterUtils.h"
#include "FurComponent.h"

static TArray< FFurSkinData* > FurSkinData;
static FCriticalSection FurSkinDataCS;

static unsigned int MaxGPUSkinBones = 256;

/** Fur Skin Vertex Blitter */
template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT, bool bExtraBoneInfluencesT>
class FFurSkinVertexBlitter : public FFurStaticVertexBlitter<TangentBasisTypeT, UVTypeT>
{
public:
	FFurSkinVertexBlitter(const FPositionVertexBuffer& InPositions, const FStaticMeshVertexBuffer& InVertices, const FColorVertexBuffer& InColors, const FSkinWeightVertexBuffer& InSkinWeights)
		: FFurStaticVertexBlitter<TangentBasisTypeT, UVTypeT>(InPositions, InVertices, InColors), SkinWeights(InSkinWeights)
	{}

	void Blit(FFurSkinVertex<TangentBasisTypeT, UVTypeT, bExtraBoneInfluencesT>& OutVertex, uint32 InVertexIndex) const
	{
		FFurStaticVertexBlitter<TangentBasisTypeT, UVTypeT>::Blit(OutVertex, InVertexIndex);

//		const auto* WeightInfo = SkinWeights.GetSkinWeightPtr<bExtraBoneInfluencesT>(InVertexIndex);
		const int32 NumInfluences = FFurSkinVertex<TangentBasisTypeT, UVTypeT, bExtraBoneInfluencesT>::NumInfluences;
		for (int32 ib = 0; ib < NumInfluences; ib++)
		{
			OutVertex.InfluenceBones[ib] = SkinWeights.GetBoneIndex(InVertexIndex, ib);
			OutVertex.InfluenceWeights[ib] = SkinWeights.GetBoneWeight(InVertexIndex, ib);
		}
	}

private:
	const FSkinWeightVertexBuffer& SkinWeights;
};

/** Vertex Factory Shader Parameters */
template <bool Physics>
class FFurSkinVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FFurSkinVertexFactoryShaderParameters<Physics>, NonVirtual);
public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		MeshOriginParameter.Bind(ParameterMap, TEXT("MeshOrigin"));
		MeshExtensionParameter.Bind(ParameterMap, TEXT("MeshExtension"));
		FurOffsetPowerParameter.Bind(ParameterMap, TEXT("FurOffsetPower"));
		MaxPhysicsOffsetLengthParameter.Bind(ParameterMap, TEXT("MaxPhysicsOffsetLength"));
		BoneMatrices.Bind(ParameterMap, TEXT("BoneMatrices"));
		PreviousBoneMatrices.Bind(ParameterMap, TEXT("PreviousBoneMatrices"));
		BoneFurOffsets.Bind(ParameterMap, TEXT("BoneFurOffsets"));
		PreviousBoneFurOffsets.Bind(ParameterMap, TEXT("PreviousBoneFurOffsets"));
	}


	void Serialize(FArchive& Ar)
	{
		Ar << MeshOriginParameter;
		Ar << MeshExtensionParameter;
		Ar << FurOffsetPowerParameter;
		Ar << MaxPhysicsOffsetLengthParameter;
		Ar << BoneMatrices;
		Ar << PreviousBoneMatrices;
		Ar << BoneFurOffsets;
		Ar << PreviousBoneFurOffsets;
	}


	/**
	* Set any shader data specific to this vertex factory
	*/
	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const class FSceneView* View,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const class FVertexFactory* VertexFactory,
		const struct FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const;

	uint32 GetSize() const { return sizeof(*this); }

private:
	LAYOUT_FIELD(FShaderParameter, MeshOriginParameter);
	LAYOUT_FIELD(FShaderParameter, MeshExtensionParameter);
	LAYOUT_FIELD(FShaderParameter, FurOffsetPowerParameter);
	LAYOUT_FIELD(FShaderParameter, MaxPhysicsOffsetLengthParameter);
	LAYOUT_FIELD(FShaderResourceParameter, BoneMatrices);
	LAYOUT_FIELD(FShaderResourceParameter, PreviousBoneMatrices);
	LAYOUT_FIELD(FShaderResourceParameter, BoneFurOffsets);
	LAYOUT_FIELD(FShaderResourceParameter, PreviousBoneFurOffsets);
};

IMPLEMENT_TYPE_LAYOUT(FFurSkinVertexFactoryShaderParameters<true>)
IMPLEMENT_TYPE_LAYOUT(FFurSkinVertexFactoryShaderParameters<false>)

/** Vertex Factory */
template<bool MorphTargets, bool Physics, bool bExtraInfluencesT>
class FFurSkinVertexFactoryBase : public FFurVertexFactory
{
	typedef FFurSkinVertexFactoryBase<MorphTargets, Physics, bExtraInfluencesT> This;

public:
	struct FShaderDataType
	{
		FShaderDataType(ERHIFeatureLevel::Type InFeatureLevel)
			: MeshOrigin(0, 0, 0)
			, MeshExtension(1, 1, 1)
			, FurOffsetPower(2.0f)
			, MaxPhysicsOffsetLength(FLT_MAX)
			, CurrentBuffer(0)
			, FeatureLevel(InFeatureLevel)
			, Discontinuous(true)
		{
		}

		/** Mesh origin and Mesh Extension for Mesh compressions **/
		/** This value will be (0, 0, 0), (1, 1, 1) relatively for non compressed meshes **/
		FVector3f MeshOrigin;
		FVector3f MeshExtension;
		float FurOffsetPower;
		float MaxPhysicsOffsetLength;

		void Init(uint32 InBoneCount)
		{
			BoneCount = InBoneCount;
		}

		// @param FrameTime from GFrameTime
		void UpdateBoneData(const TArray<FMatrix>& ReferenceToLocalMatrices, const TArray<FVector>& LinearOffsets, const TArray<FVector>& AngularOffsets,
			const TArray<FMatrix>& LastTransformations, const TArray<FBoneIndexType>& BoneMap, bool InDiscontinuous, ERHIFeatureLevel::Type FeatureLevel);

		void InitDynamicRHI();

		void ReleaseBoneData()
		{
			ensure(IsInRenderingThread());

			UniformBuffer.SafeRelease();

			for (uint32 i = 0; i < 2; ++i)
			{
				BoneBuffer[i].SafeRelease();
				BoneFurOffsetsBuffer[i].SafeRelease();
			}
		}

		// if FeatureLevel < ERHIFeatureLevel::ES3_1
		FUniformBufferRHIRef GetUniformBuffer() const
		{
			return UniformBuffer;
		}

		// @param bPrevious true:previous, false:current
		// @param FrameNumber usually from View.Family->FrameNumber
		const FVertexBufferAndSRV& GetBoneBufferForReading(bool bPrevious) const
		{
			const FVertexBufferAndSRV* RetPtr = &GetBoneBufferInternal(bPrevious);

			if (!RetPtr->VertexBufferRHI.IsValid())
			{
				// this only should happen if we request the old data
				check(bPrevious);

				// if we don't have any old data we use the current one
				RetPtr = &GetBoneBufferInternal(false);

				// at least the current one needs to be valid when reading
				check(RetPtr->VertexBufferRHI.IsValid());
			}

			return *RetPtr;
		}

		// @param bPrevious true:previous, false:current
		// @param FrameNumber usually from View.Family->FrameNumber
		// @return IsValid() can fail, then you have to create the buffers first (or if the size changes)
		FVertexBufferAndSRV& GetBoneBufferForWriting(bool bPrevious)
		{
			const FShaderDataType* This = (const FShaderDataType*)this;

			// non const version maps to const version
			return (FVertexBufferAndSRV&)This->GetBoneBufferInternal(bPrevious);
		}

		const FVertexBufferAndSRV& GetBoneFurOffsetsBufferForReading(bool bPrevious) const
		{
			const FVertexBufferAndSRV* RetPtr = &GetBoneFurOffsetsBufferInternal(bPrevious);

			if (!RetPtr->VertexBufferRHI.IsValid())
			{
				// this only should happen if we request the old data
				check(bPrevious);

				// if we don't have any old data we use the current one
				RetPtr = &GetBoneFurOffsetsBufferInternal(false);

				// at least the current one needs to be valid when reading
				check(RetPtr->VertexBufferRHI.IsValid());
			}

			return *RetPtr;
		}

		FVertexBufferAndSRV& GetBoneFurOffsetsBufferForWriting(bool bPrevious)
		{
			const FShaderDataType* This = (const FShaderDataType*)this;

			// non const version maps to const version
			return (FVertexBufferAndSRV&)This->GetBoneFurOffsetsBufferInternal(bPrevious);
		}

	private:
		// double buffered bone positions+orientations to support normal rendering and velocity (new-old position) rendering
		FVertexBufferAndSRV BoneBuffer[2];
		FVertexBufferAndSRV BoneFurOffsetsBuffer[2];
		// 0 / 1 to index into BoneBuffer
		uint32 CurrentBuffer;
		// if FeatureLevel < ERHIFeatureLevel::ES3_1
		FUniformBufferRHIRef UniformBuffer;
		uint32 BoneCount;
		ERHIFeatureLevel::Type FeatureLevel;
		bool Discontinuous;

		void GoToNextFrame(bool InDiscontinuous);

		// to support GetBoneBufferForWriting() and GetBoneBufferForReading()
		// @param bPrevious true:previous, false:current
		// @param FrameNumber usually from View.Family->FrameNumber
		// @return might not pass the IsValid() 
		const FVertexBufferAndSRV& GetBoneBufferInternal(bool bPrevious) const
		{
			check(IsInParallelRenderingThread());

			if (Discontinuous)
			{
				bPrevious = false;
			}

			uint32 BufferIndex = CurrentBuffer ^ (uint32)bPrevious;

			const FVertexBufferAndSRV& Ret = BoneBuffer[BufferIndex];
			return Ret;
		}

		const FVertexBufferAndSRV& GetBoneFurOffsetsBufferInternal(bool bPrevious) const
		{
			check(IsInParallelRenderingThread());

			if (Discontinuous)
			{
				bPrevious = false;
			}

			uint32 BufferIndex = CurrentBuffer ^ (uint32)bPrevious;

			const FVertexBufferAndSRV& Ret = BoneFurOffsetsBuffer[BufferIndex];
			return Ret;
		}
	};

	FFurSkinVertexFactoryBase(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurVertexFactory(InFeatureLevel)
		, ShaderData(InFeatureLevel)
	{
	}

	struct FDataType
	{
		FVertexStreamComponent PositionComponent;
		FVertexStreamComponent TangentBasisComponents[2];
		TArray<FVertexStreamComponent, TFixedAllocator<MAX_TEXCOORDS>> TextureCoordinates;
		FVertexStreamComponent ColorComponent;
		FVertexStreamComponent BoneIndices;
		FVertexStreamComponent BoneIndicesExtra;
		FVertexStreamComponent BoneWeights;
		FVertexStreamComponent BoneWeightsExtra;
		FVertexStreamComponent FurOffset;

		FVertexStreamComponent DeltaPosition;
		FVertexStreamComponent DeltaTangentZ;
	};

	template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT>
	void Init(const FFurVertexBuffer* VertexBuffer, const FVertexBuffer* MorphVertexBuffer, uint32 BoneCount)
	{
		typedef FFurSkinVertex<TangentBasisTypeT, UVTypeT, bExtraInfluencesT> VertexType;
		ShaderData.Init(BoneCount);
		ENQUEUE_RENDER_COMMAND(InitProceduralMeshVertexFactory)
			([this, VertexBuffer, MorphVertexBuffer](FRHICommandListImmediate& RHICmdList) {
				const auto TangentElementType = TStaticMeshVertexTangentTypeSelector<TangentBasisTypeT>::VertexElementType;
				const auto UvElementType = UVTypeT == EStaticMeshVertexUVType::HighPrecision ? VET_Float2 : VET_Half2;

				// Initialize the vertex factory's stream components.
				FDataType NewData;
				NewData.PositionComponent = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(VertexBuffer, VertexType, Position, VET_Float3);
				NewData.TextureCoordinates.Add(FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, UV0), sizeof(VertexType), UvElementType));
				NewData.TextureCoordinates.Add(FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, UV1), sizeof(VertexType), VET_Float2));
				NewData.TextureCoordinates.Add(FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, UV2), sizeof(VertexType), VET_Float2));
				NewData.TangentBasisComponents[0] = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(VertexBuffer, VertexType, TangentX, TangentElementType);
				NewData.TangentBasisComponents[1] = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(VertexBuffer, VertexType, TangentZ, TangentElementType);
				NewData.ColorComponent = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(VertexBuffer, VertexType, Color, VET_Color);
				NewData.BoneIndices = FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, InfluenceBones), sizeof(VertexType), VET_UByte4);
				if (bExtraInfluencesT)
					NewData.BoneIndicesExtra = FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, InfluenceBones) + 4, sizeof(VertexType), VET_UByte4);
				NewData.BoneWeights = FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, InfluenceWeights), sizeof(VertexType), VET_UByte4N);
				if (bExtraInfluencesT)
					NewData.BoneWeightsExtra = FVertexStreamComponent(VertexBuffer, STRUCT_OFFSET(VertexType, InfluenceWeights) + 4, sizeof(VertexType), VET_UByte4N);
				NewData.FurOffset = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(VertexBuffer, VertexType, FurOffset, VET_Float3);

				if (MorphTargets)
				{
					NewData.DeltaPosition = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(MorphVertexBuffer, FMorphGPUSkinVertex, DeltaPosition, VET_Float3);
					NewData.DeltaTangentZ = STRUCTMEMBER_VERTEXSTREAMCOMPONENT(MorphVertexBuffer, FMorphGPUSkinVertex, DeltaTangentZ, VET_Float3);
				}

				SetData(NewData);
			});
	}

	static FVertexFactoryShaderParameters* ConstructShaderParameters(EShaderFrequency ShaderFrequency)
	{
		return (ShaderFrequency == SF_Vertex) ? new FFurSkinVertexFactoryShaderParameters<Physics>() : NULL;
	}


	static bool ShouldCache(const FVertexFactoryShaderPermutationParameters& Parameters)
	{
		return (Parameters.MaterialParameters.bIsUsedWithSkeletalMesh || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
	}

	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
//		Super::ModifyCompilationEnvironment(Platform, Material, OutEnvironment);
		if (MorphTargets)
			OutEnvironment.SetDefine(TEXT("GPUSKIN_MORPH_BLEND"), TEXT("1"));
		if (Physics)
			OutEnvironment.SetDefine(TEXT("GFUR_PHYSICS"), TEXT("1"));
		if (bExtraInfluencesT)
			OutEnvironment.SetDefine(TEXT("GPUSKIN_USE_EXTRA_INFLUENCES"), TEXT("1"));
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
	{
		if (Parameters.MaterialParameters.bIsUsedWithSkeletalMesh)
			return true;
		if (Parameters.MaterialParameters.bIsSpecialEngineMaterial)
			return true;
		return (MorphTargets ? Parameters.MaterialParameters.bIsUsedWithMorphTargets : false);
	}

	void SetData(const FDataType& InData)
	{
		Data = InData;
		FVertexFactory::UpdateRHI();
	}

	void AddVertexElements(FDataType& InData, FVertexDeclarationElementList& OutElements)
	{
		OutElements.Add(AccessStreamComponent(InData.PositionComponent, 0));

		OutElements.Add(AccessStreamComponent(InData.TangentBasisComponents[0], 1));
		OutElements.Add(AccessStreamComponent(InData.TangentBasisComponents[1], 2));

		if (InData.TextureCoordinates.Num())
		{
			const uint8 BaseTexCoordAttribute = 5;
			for (int32 CoordinateIndex = 0; CoordinateIndex < InData.TextureCoordinates.Num(); CoordinateIndex++)
			{
				OutElements.Add(AccessStreamComponent(
					InData.TextureCoordinates[CoordinateIndex], BaseTexCoordAttribute + CoordinateIndex));
			}

			for (int32 CoordinateIndex = InData.TextureCoordinates.Num(); CoordinateIndex < MAX_TEXCOORDS;
				CoordinateIndex++)
			{
				OutElements.Add(AccessStreamComponent(InData.TextureCoordinates[InData.TextureCoordinates.Num() - 1],
					BaseTexCoordAttribute + CoordinateIndex));
			}
		}

		if (InData.ColorComponent.VertexBuffer)
		{
			OutElements.Add(AccessStreamComponent(InData.ColorComponent, 13));
		}
		else
		{
			FVertexStreamComponent NullColorComponent(&GNullColorVertexBuffer, 0, 0, VET_Color);
			OutElements.Add(AccessStreamComponent(NullColorComponent, 13));
		}
		OutElements.Add(AccessStreamComponent(InData.BoneIndices, 3));
		OutElements.Add(AccessStreamComponent(InData.BoneWeights, 4));
		OutElements.Add(AccessStreamComponent(InData.FurOffset, 12));

		if (MorphTargets)
		{
			OutElements.Add(AccessStreamComponent(InData.DeltaPosition, 9));
			OutElements.Add(AccessStreamComponent(InData.DeltaTangentZ, 10));
		}

		if (bExtraInfluencesT)
		{
			OutElements.Add(AccessStreamComponent(InData.BoneIndicesExtra, 14));
			OutElements.Add(AccessStreamComponent(InData.BoneWeightsExtra, 15));
		}
	}

	void InitRHI() override
	{
		// list of declaration items
		FVertexDeclarationElementList Elements;
		AddVertexElements(Data, Elements);

		// create the actual device decls
		InitDeclaration(Elements);
	}

	void InitDynamicRHI() override
	{
		FVertexFactory::InitDynamicRHI();
		ShaderData.InitDynamicRHI();
	}

	void ReleaseDynamicRHI() override
	{
		FVertexFactory::ReleaseDynamicRHI();
		ShaderData.ReleaseBoneData();
	}

	void UpdateSkeletonShaderData(float InFurOffsetPower, float InMaxPhysicsOffsetLength, const TArray<FMatrix>& InReferenceToLocal, const TArray<FVector>& InLinearOffsets, const TArray<FVector>& InAngularOffsets,
		const TArray<FMatrix>& InTransformations, const TArray<FBoneIndexType>& InBoneMap, bool InDiscontinuous, ERHIFeatureLevel::Type InFeatureLevel) override
	{
		ShaderData.FurOffsetPower = InFurOffsetPower;
		ShaderData.MaxPhysicsOffsetLength = InMaxPhysicsOffsetLength;
		ShaderData.UpdateBoneData(InReferenceToLocal, InLinearOffsets, InAngularOffsets, InTransformations, InBoneMap, InDiscontinuous, InFeatureLevel);
	}

	FDataType Data;
	FShaderDataType ShaderData;
};

class FMorphPhysicsExtraInfluencesFurSkinVertexFactory : public FFurSkinVertexFactoryBase<true, true, true>
{
	DECLARE_VERTEX_FACTORY_TYPE(FMorphPhysicsExtraInfluencesFurSkinVertexFactory);
public:
	FMorphPhysicsExtraInfluencesFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<true, true, true>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<true, true, true>::Init;
};

class FPhysicsExtraInfluencesFurSkinVertexFactory : public FFurSkinVertexFactoryBase<false, true, true>
{
	DECLARE_VERTEX_FACTORY_TYPE(FPhysicsExtraInfluencesFurSkinVertexFactory);
public:
	FPhysicsExtraInfluencesFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<false, true, true>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<false, true, true>::Init;
};

class FMorphExtraInfluencesFurSkinVertexFactory : public FFurSkinVertexFactoryBase<true, false, true>
{
	DECLARE_VERTEX_FACTORY_TYPE(FMorphExtraInfluencesFurSkinVertexFactory);
public:
	FMorphExtraInfluencesFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<true, false, true>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<true, false, true>::Init;
};

class FExtraInfluencesFurSkinVertexFactory : public FFurSkinVertexFactoryBase<false, false, true>
{
	DECLARE_VERTEX_FACTORY_TYPE(FExtraInfluencesFurSkinVertexFactory);
public:
	FExtraInfluencesFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<false, false, true>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<false, false, true>::Init;
};

class FMorphPhysicsFurSkinVertexFactory : public FFurSkinVertexFactoryBase<true, true, false>
{
	DECLARE_VERTEX_FACTORY_TYPE(FMorphPhysicsFurSkinVertexFactory);
public:
	FMorphPhysicsFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<true, true, false>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<true, true, false>::Init;
};

class FPhysicsFurSkinVertexFactory : public FFurSkinVertexFactoryBase<false, true, false>
{
	DECLARE_VERTEX_FACTORY_TYPE(FPhysicsFurSkinVertexFactory);
public:
	FPhysicsFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<false, true, false>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<false, true, false>::Init;
};

class FMorphFurSkinVertexFactory : public FFurSkinVertexFactoryBase<true, false, false>
{
	DECLARE_VERTEX_FACTORY_TYPE(FMorphFurSkinVertexFactory);
public:
	FMorphFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<true, false, false>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<true, false, false>::Init;
};

class FFurSkinVertexFactory : public FFurSkinVertexFactoryBase<false, false, false>
{
	DECLARE_VERTEX_FACTORY_TYPE(FFurSkinVertexFactory);
public:
	FFurSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FFurSkinVertexFactoryBase<false, false, false>(InFeatureLevel)
	{
	}

	using FFurSkinVertexFactoryBase<false, false, false>::Init;
};

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMorphPhysicsExtraInfluencesFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<true>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FPhysicsExtraInfluencesFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<true>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMorphExtraInfluencesFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<false>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FExtraInfluencesFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<false>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMorphPhysicsFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<true>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FPhysicsFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<true>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FMorphFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<false>);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FFurSkinVertexFactory, SF_Vertex, FFurSkinVertexFactoryShaderParameters<false>);

IMPLEMENT_VERTEX_FACTORY_TYPE(FMorphPhysicsExtraInfluencesFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FPhysicsExtraInfluencesFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FMorphExtraInfluencesFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FExtraInfluencesFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FMorphPhysicsFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FPhysicsFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FMorphFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);
IMPLEMENT_VERTEX_FACTORY_TYPE(FFurSkinVertexFactory, "/Plugin/gFur/Private/GFurFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos);

#if WITH_EDITORONLY_DATA
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FBoneMatricesUniformShaderParameters, "BonesFur");
#endif // WITH_EDITORONLY_DATA

static FBoneMatricesUniformShaderParameters GBoneUniformStruct;

template<bool MorphTargets, bool Physics, bool ExtraInfluences>
void FFurSkinVertexFactoryBase<MorphTargets, Physics, ExtraInfluences>::FShaderDataType::GoToNextFrame(bool InDiscontinuous)
{
	CurrentBuffer = 1 - CurrentBuffer;
	Discontinuous = InDiscontinuous;
}

template<bool MorphTargets, bool Physics, bool ExtraInfluences>
void FFurSkinVertexFactoryBase<MorphTargets, Physics, ExtraInfluences>::FShaderDataType::UpdateBoneData(const TArray<FMatrix>& ReferenceToLocalMatrices, const TArray<FVector>& LinearOffsets, const TArray<FVector>& AngularOffsets,
	const TArray<FMatrix>& LastTransformations, const TArray<FBoneIndexType>& BoneMap, bool InDiscontinuous, ERHIFeatureLevel::Type InFeatureLevel)
{
	const uint32 NumBones = BoneMap.Num();
	check(NumBones <= MaxGPUSkinBones);
	float* ChunkMatrices = nullptr;
	FVector4f* Offsets = nullptr;

	FVertexBufferAndSRV* CurrentBoneBuffer = 0;
	FVertexBufferAndSRV* CurrentBoneFurOffsetsBuffer = 0;

	if (InFeatureLevel >= ERHIFeatureLevel::ES3_1)
	{
		check(IsInRenderingThread());
		GoToNextFrame(InDiscontinuous);

		CurrentBoneBuffer = &GetBoneBufferForWriting(false);

		uint32 NumVectors = NumBones * 3;
		check(NumVectors <= (MaxGPUSkinBones * 3));
		uint32 VectorArraySize = NumVectors * sizeof(FVector4f);

		uint32 OffsetArraySize = NumBones * 3 * sizeof(FVector4f);

		if (!IsValidRef(*CurrentBoneBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(VectorArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*CurrentBoneBuffer = Buffer;
			check(IsValidRef(*CurrentBoneBuffer));
		}

		CurrentBoneFurOffsetsBuffer = &GetBoneFurOffsetsBufferForWriting(false);
		if (!IsValidRef(*CurrentBoneFurOffsetsBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(OffsetArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*CurrentBoneFurOffsetsBuffer = Buffer;
			check(IsValidRef(*CurrentBoneFurOffsetsBuffer));
		}

		if (NumBones)
		{
			ChunkMatrices = (float*)RHILockBuffer(CurrentBoneBuffer->VertexBufferRHI, 0, VectorArraySize, RLM_WriteOnly);
			Offsets = (FVector4f*)RHILockBuffer(CurrentBoneFurOffsetsBuffer->VertexBufferRHI, 0, OffsetArraySize, RLM_WriteOnly);
		}
	}
	else
	{
		if (NumBones)
		{
			check(NumBones * sizeof(float) * 12 <= sizeof(GBoneUniformStruct));
			ChunkMatrices = (float*)&GBoneUniformStruct;
		}
	}

	if (InFeatureLevel >= ERHIFeatureLevel::ES3_1)
	{
		//FSkinMatrix3x4 is sizeof() == 48
		// PLATFORM_CACHE_LINE_SIZE (128) / 48 = 2.6
		//  sizeof(FMatrix) == 64
		// PLATFORM_CACHE_LINE_SIZE (128) / 64 = 2
		const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
		for (uint32 BoneIdx = 0; BoneIdx < NumBones; BoneIdx++)
		{
			const FBoneIndexType RefToLocalIdx = BoneMap[BoneIdx];
			FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + RefToLocalIdx + PreFetchStride);
			FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + RefToLocalIdx + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

			float* BoneMat = ChunkMatrices + BoneIdx * 12;
			const FMatrix44f RefToLocal = FMatrix44f(ReferenceToLocalMatrices[RefToLocalIdx]);
			RefToLocal.To3x4MatrixTranspose(BoneMat);

			Offsets[BoneIdx * 3] = FVector3f(LinearOffsets[RefToLocalIdx]);
			Offsets[BoneIdx * 3 + 1] = FVector3f(AngularOffsets[RefToLocalIdx]);
			Offsets[BoneIdx * 3 + 2] = FVector3f(LastTransformations[RefToLocalIdx].GetOrigin());
		}
	}
	else
	{
		const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
		for (uint32 BoneIdx = 0; BoneIdx < NumBones; BoneIdx++)
		{
			const FBoneIndexType RefToLocalIdx = BoneMap[BoneIdx];
			FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + RefToLocalIdx + PreFetchStride);
			FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + RefToLocalIdx + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

			float* BoneMat = ChunkMatrices + BoneIdx * 12;
			const FMatrix44f RefToLocal = FMatrix44f(ReferenceToLocalMatrices[RefToLocalIdx]);
			RefToLocal.To3x4MatrixTranspose(BoneMat);
		}
	}
	if (InFeatureLevel >= ERHIFeatureLevel::ES3_1)
	{
		if (NumBones)
		{
			check(CurrentBoneBuffer);
			RHIUnlockBuffer(CurrentBoneBuffer->VertexBufferRHI);
			check(CurrentBoneFurOffsetsBuffer);
			RHIUnlockBuffer(CurrentBoneFurOffsetsBuffer->VertexBufferRHI);
		}
	}
	else
	{
		UniformBuffer = RHICreateUniformBuffer(&GBoneUniformStruct, &FBoneMatricesUniformShaderParameters::StaticStructMetadata.GetLayout(), UniformBuffer_MultiFrame);
	}
}

template<bool MorphTargets, bool Physics, bool ExtraInfluences>
void FFurSkinVertexFactoryBase<MorphTargets, Physics, ExtraInfluences>::FShaderDataType::InitDynamicRHI()
{
	const uint32 NumBones = BoneCount;
	check(NumBones <= MaxGPUSkinBones);
	float* ChunkMatrices = nullptr;

	if (FeatureLevel >= ERHIFeatureLevel::ES3_1)
	{
		check(IsInRenderingThread());

		uint32 NumVectors = NumBones * 3;
		check(NumVectors <= (MaxGPUSkinBones * 3));
		uint32 VectorArraySize = NumVectors * sizeof(FVector4f);

		uint32 OffsetArraySize = NumBones * 3 * sizeof(FVector4f);

		FVertexBufferAndSRV* CurrentBoneBuffer = &GetBoneBufferForWriting(false);
		if (!IsValidRef(*CurrentBoneBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(VectorArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*CurrentBoneBuffer = Buffer;
			check(IsValidRef(*CurrentBoneBuffer));
		}

		FVertexBufferAndSRV* PreviousBoneBuffer = &GetBoneBufferForWriting(true);
		if (!IsValidRef(*PreviousBoneBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(VectorArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*PreviousBoneBuffer = Buffer;
			check(IsValidRef(*PreviousBoneBuffer));
		}

		FVertexBufferAndSRV* CurrentBoneFurOffsetsBuffer = &GetBoneFurOffsetsBufferForWriting(false);
		if (!IsValidRef(*CurrentBoneFurOffsetsBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(OffsetArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*CurrentBoneFurOffsetsBuffer = Buffer;
			check(IsValidRef(*CurrentBoneFurOffsetsBuffer));
		}

		FVertexBufferAndSRV* PreviousBoneFurOffsetsBuffer = &GetBoneFurOffsetsBufferForWriting(true);
		if (!IsValidRef(*PreviousBoneFurOffsetsBuffer))
		{
			FVertexBufferAndSRV Buffer;
			FRHIResourceCreateInfo CreateInfo(L"FurVertexBuffer");
			Buffer.VertexBufferRHI = RHICreateVertexBuffer(OffsetArraySize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
			Buffer.VertexBufferSRV = RHICreateShaderResourceView(Buffer.VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
			*PreviousBoneFurOffsetsBuffer = Buffer;
			check(IsValidRef(*PreviousBoneFurOffsetsBuffer));
		}
	}
	else
	{
		if (NumBones)
		{
			check(NumBones * sizeof(float) * 12 <= sizeof(GBoneUniformStruct));
			ChunkMatrices = (float*)&GBoneUniformStruct;
		}
	}

	if (FeatureLevel >= ERHIFeatureLevel::ES3_1)
	{
	}
	else
	{
		UniformBuffer = RHICreateUniformBuffer(&GBoneUniformStruct, &FBoneMatricesUniformShaderParameters::StaticStructMetadata.GetLayout(), UniformBuffer_MultiFrame);
	}
}

template<bool Physics>
void FFurSkinVertexFactoryShaderParameters<Physics>::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const class FSceneView* View,
	const class FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const class FVertexFactory* VertexFactory,
	const struct FMeshBatchElement& BatchElement,
	class FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	FFurSkinVertexFactory::FShaderDataType& ShaderData = ((FFurSkinVertexFactory*)VertexFactory)->ShaderData;

	ShaderBindings.Add(MeshOriginParameter, ShaderData.MeshOrigin);
	ShaderBindings.Add(MeshExtensionParameter, ShaderData.MeshExtension);
	ShaderBindings.Add(FurOffsetPowerParameter, ShaderData.FurOffsetPower);
	ShaderBindings.Add(MaxPhysicsOffsetLengthParameter, ShaderData.MaxPhysicsOffsetLength);

//		const auto FeatureLevel = View.GetFeatureLevel();

	if (BoneMatrices.IsBound())
	{
		auto CurrentData = ShaderData.GetBoneBufferForReading(false).VertexBufferSRV;
		ShaderBindings.Add(BoneMatrices, CurrentData);
	}
	if (PreviousBoneMatrices.IsBound())
	{
		// todo: Maybe a check for PreviousData!=CurrentData would save some performance (when objects don't have velocty yet) but removing the bool also might save performance

		auto PreviousData = ShaderData.GetBoneBufferForReading(true).VertexBufferSRV;
		ShaderBindings.Add(PreviousBoneMatrices, PreviousData);
	}

	if (Physics)
	{
		if (BoneFurOffsets.IsBound())
		{
			auto CurrentData = ShaderData.GetBoneFurOffsetsBufferForReading(false).VertexBufferSRV;
			ShaderBindings.Add(BoneFurOffsets, CurrentData);
		}
		if (PreviousBoneFurOffsets.IsBound())
		{
			auto PreviousData = ShaderData.GetBoneFurOffsetsBufferForReading(true).VertexBufferSRV;
			ShaderBindings.Add(PreviousBoneFurOffsets, PreviousData);
		}
	}
	else
	{
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FBoneMatricesUniformShaderParameters>(), ShaderData.GetUniformBuffer());
	}
}

/** Fur Skin Data */
FFurSkinData* FFurSkinData::CreateFurData(int32 InFurLayerCount, int32 InLod, UGFurComponent* InFurComponent)
{
	check(InFurLayerCount >= MinimalFurLayerCount && InFurLayerCount <= MaximalFurLayerCount);

	FScopeLock lock(&FurSkinDataCS);

	/*
	 * 调用FFurSkinData::Compare对比静态数组FurSkinData
	 * 相同就添加引用返回
	 */
	for (FFurSkinData* Data : FurSkinData)
	{
		if (Data->Compare(InFurLayerCount, InLod, InFurComponent))
		{
			Data->RefCount++;
			return Data;
		}
	}
/*	for (FFurSkinData* Data : FurSkinData)
	{
		if (Data->RefCount == 0 && Data->Similar(InLod, InFurComponent))
		{
			Data->Set(InFurLayerCount, InLod, InFurComponent);
			Data->BuildFur(BuildType::Minimal);
			Data->RefCount++;
			return Data;
		}
	}*/

	FFurSkinData* Data = new FFurSkinData();
	/*
	 * 从组件中获得SkeletalMesh和GuideMeshes，
	 * 并视配置情况，生成UFurSplines
	 */
	Data->Set(InFurLayerCount, InLod, InFurComponent);
	
	/*
	 * BuildFur是一组模板方法，最终调用到BuildFur<T1,T2,T3>
	 * 三个模板参数影响到后续模板类型FFurSkinVertexFactoryBase的实例化，
	 * 对应其MorphTargets, Physics, bExtraInfluences的true or false
	 */
	Data->BuildFur(BuildType::Full);

	/*
	 * 加入到全局静态数组里面缓存
	 */
	FurSkinData.Add(Data);
	return Data;
}

void FFurSkinData::DestroyFurData(const TArray<FFurData*>& InFurDataArray)
{
	FScopeLock lock(&FurSkinDataCS);

	for (auto* Data : InFurDataArray)
		((FFurSkinData*)Data)->RefCount--;

	StartFurDataCleanupTask([]() {

		FScopeLock lock(&FurSkinDataCS);

		for (int32 i = FurSkinData.Num() - 1; i >= 0; i--)
		{
			FFurSkinData* Data = FurSkinData[i];
			if (Data->RefCount == 0)
			{
				FurSkinData.RemoveAt(i);
				ENQUEUE_RENDER_COMMAND(ReleaseDataCommand)([Data](FRHICommandListImmediate& RHICmdList) { delete Data; });
			}
		}
	});
}

void FFurSkinData::CreateVertexFactories(TArray<FFurVertexFactory*>& VertexFactories, FVertexBuffer* InMorphVertexBuffer, bool InPhysics, ERHIFeatureLevel::Type InFeatureLevel)
{
	auto CreateVertexFactory = [&](const FFurData::FSection& s, auto* vf) {
		if (bUseHighPrecisionTangentBasis)
		{
			if (bUseFullPrecisionUVs)
				vf->template Init<EStaticMeshVertexTangentBasisType::HighPrecision, EStaticMeshVertexUVType::HighPrecision>(&VertexBuffer, InMorphVertexBuffer, s.NumBones);
			else
				vf->template Init<EStaticMeshVertexTangentBasisType::HighPrecision, EStaticMeshVertexUVType::Default>(&VertexBuffer, InMorphVertexBuffer, s.NumBones);
		}
		else
		{
			if (bUseFullPrecisionUVs)
				vf->template Init<EStaticMeshVertexTangentBasisType::Default, EStaticMeshVertexUVType::HighPrecision>(&VertexBuffer, InMorphVertexBuffer, s.NumBones);
			else
				vf->template Init<EStaticMeshVertexTangentBasisType::Default, EStaticMeshVertexUVType::Default>(&VertexBuffer, InMorphVertexBuffer, s.NumBones);
		}
		BeginInitResource(vf);
		VertexFactories.Add(vf);
	};

	for (auto& s : Sections)
	{
		if (InPhysics && InFeatureLevel >= ERHIFeatureLevel::ES3_1)
		{
			if (InMorphVertexBuffer)
			{
				if (HasExtraBoneInfluences)
					CreateVertexFactory(s, new FMorphPhysicsExtraInfluencesFurSkinVertexFactory(InFeatureLevel));
				else
					CreateVertexFactory(s, new FMorphPhysicsFurSkinVertexFactory(InFeatureLevel));
			}
			else
			{
				if (HasExtraBoneInfluences)
					CreateVertexFactory(s, new FPhysicsExtraInfluencesFurSkinVertexFactory(InFeatureLevel));
				else
					CreateVertexFactory(s, new FPhysicsFurSkinVertexFactory(InFeatureLevel));
			}
		}
		else
		{
			if (InMorphVertexBuffer)
			{
				if (HasExtraBoneInfluences)
					CreateVertexFactory(s, new FMorphExtraInfluencesFurSkinVertexFactory(InFeatureLevel));
				else
					CreateVertexFactory(s, new FMorphFurSkinVertexFactory(InFeatureLevel));
			}
			else
			{
				if (HasExtraBoneInfluences)
					CreateVertexFactory(s, new FExtraInfluencesFurSkinVertexFactory(InFeatureLevel));
				else
					CreateVertexFactory(s, new FFurSkinVertexFactory(InFeatureLevel));
			}
		}
	}
}

FFurSkinData::~FFurSkinData()
{
	UnbindChangeDelegates();

#if WITH_EDITORONLY_DATA
	if (SkeletalMesh)
		SkeletalMesh->RemoveFromRoot();
	for (USkeletalMesh* Mesh : GuideMeshes)
		Mesh->RemoveFromRoot();
#endif // WITH_EDITORONLY_DATA
}

void FFurSkinData::UnbindChangeDelegates()
{
#if WITH_EDITORONLY_DATA
	if (FurSplinesAssigned)
	{
		if (FurSplinesChangeHandle.IsValid())
		{
			FurSplinesAssigned->OnSplinesChanged.Remove(FurSplinesChangeHandle);
			FurSplinesChangeHandle.Reset();
		}
		if (FurSplinesCombHandle.IsValid())
		{
			FurSplinesAssigned->OnSplinesCombed.Remove(FurSplinesCombHandle);
			FurSplinesCombHandle.Reset();
		}
	}
	if (SkeletalMesh && SkeletalMeshChangeHandle.IsValid())
	{
		SkeletalMesh->GetOnMeshChanged().Remove(SkeletalMeshChangeHandle);
		SkeletalMeshChangeHandle.Reset();
	}
	for (int32 i = 0; i < GuideMeshes.Num() && i < GuideMeshesChangeHandles.Num(); i++)
	{
		if (GuideMeshes[i])
			GuideMeshes[i]->GetOnMeshChanged().Remove(GuideMeshesChangeHandles[i]);
	}
	GuideMeshesChangeHandles.Reset();
#endif // WITH_EDITORONLY_DATA
}

void FFurSkinData::Set(int32 InFurLayerCount, int32 InLod, class UGFurComponent* InFurComponent)
{
	UnbindChangeDelegates();
#if WITH_EDITORONLY_DATA
	if (SkeletalMesh)
		SkeletalMesh->RemoveFromRoot();
	for (USkeletalMesh* Mesh : GuideMeshes)
		Mesh->RemoveFromRoot();
#endif // WITH_EDITORONLY_DATA

	/*
	 * 从组件上获得毛发渲染相关的配置数据
	 * 如Lod、FurLayerCount、FurLength、ShellBias等
	 */
	FFurData::Set(InFurLayerCount, InLod, InFurComponent);

	SkeletalMesh = InFurComponent->SkeletalGrowMesh;
	GuideMeshes = InFurComponent->SkeletalGuideMeshes;
#if WITH_EDITORONLY_DATA
	if (SkeletalMesh)
		SkeletalMesh->AddToRoot();
	for (auto* Mesh : InFurComponent->SkeletalGuideMeshes)
		Mesh->AddToRoot();
#endif // WITH_EDITORONLY_DATA

	check(SkeletalMesh);

	/*
	 * 如果没有指定FurSplines，组件传入的引导Mesh数量大于0，需要生成FurSplines
	 * 调用GenerateSplines()生成引导网格
	 */
	if (FurSplinesAssigned == NULL && GuideMeshes.Num() > 0)
	{
		if (FurSplinesGenerated)
			FurSplinesGenerated->ConditionalBeginDestroy();
		FurSplinesGenerated = NewObject<UFurSplines>();
		GenerateSplines(FurSplinesGenerated, SkeletalMesh, InLod, GuideMeshes);
		FurSplinesUsed = FurSplinesGenerated;
	}

#if WITH_EDITORONLY_DATA
	SkeletalMeshChangeHandle = SkeletalMesh->GetOnMeshChanged().AddLambda([this]() { BuildFur(BuildType::Full); });
	if (FurSplinesAssigned)
	{
		FurSplinesChangeHandle = FurSplinesAssigned->OnSplinesChanged.AddLambda([this]() { BuildFur(BuildType::Splines); });
		FurSplinesCombHandle = FurSplinesAssigned->OnSplinesCombed.AddLambda([this](const TArray<uint32>& VertexSet) { BuildFur(VertexSet); });
	}
	else if (GuideMeshes.Num() > 0)
	{
		for (const auto& GuideMesh : GuideMeshes)
		{
			if (GuideMesh)
			{
				auto Handle = GuideMesh->GetOnMeshChanged().AddLambda([this, InLod]() {
					if (FurSplinesGenerated)
						FurSplinesGenerated->ConditionalBeginDestroy();
					FurSplinesGenerated = NewObject<UFurSplines>();
					GenerateSplines(FurSplinesGenerated, SkeletalMesh, InLod, GuideMeshes);
					FurSplinesUsed = FurSplinesGenerated;
					BuildFur(BuildType::Splines);
				});
				GuideMeshesChangeHandles.Add(Handle);
			}
			else
			{
				GuideMeshesChangeHandles.Emplace();
			}
		}
	}
#endif // WITH_EDITORONLY_DATA
}

bool FFurSkinData::Compare(int32 InFurLayerCount, int32 InLod, class UGFurComponent* InFurComponent)
{
	return FFurData::Compare(InFurLayerCount, InLod, InFurComponent) && SkeletalMesh == InFurComponent->SkeletalGrowMesh && GuideMeshes == InFurComponent->SkeletalGuideMeshes;
}

bool FFurSkinData::Similar(int32 InLod, class UGFurComponent* InFurComponent)
{
	return FFurData::Similar(InLod, InFurComponent) && SkeletalMesh == InFurComponent->SkeletalGrowMesh && GuideMeshes == InFurComponent->SkeletalGuideMeshes;
}

void FFurSkinData::BuildFur(BuildType Build)
{
	auto* SkeletalMeshResource = SkeletalMesh->GetResourceForRendering();
	check(SkeletalMeshResource);

	const FSkeletalMeshLODRenderData& LodRenderData = SkeletalMeshResource->LODRenderData[Lod];
	if (LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetUseHighPrecisionTangentBasis())
		BuildFur<EStaticMeshVertexTangentBasisType::HighPrecision>(LodRenderData, Build);
	else
		BuildFur<EStaticMeshVertexTangentBasisType::Default>(LodRenderData, Build);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, BuildType Build)
{
	if (LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetUseFullPrecisionUVs())
		BuildFur<TangentBasisTypeT, EStaticMeshVertexUVType::HighPrecision>(LodRenderData, Build);
	else
		BuildFur<TangentBasisTypeT, EStaticMeshVertexUVType::Default>(LodRenderData, Build);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, BuildType Build)
{
	if (LodRenderData.SkinWeightVertexBuffer.GetMaxBoneInfluences() > 4)
		BuildFur<TangentBasisTypeT, UVTypeT, true>(LodRenderData, Build);
	else
		BuildFur<TangentBasisTypeT, UVTypeT, false>(LodRenderData, Build);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT, bool bExtraBoneInfluencesT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, BuildType Build)
{
	typedef FFurSkinVertex<TangentBasisTypeT, UVTypeT, bExtraBoneInfluencesT> VertexType;

	/*
	 * 根据模板参数设置FFurData的成员变量bUseHighPrecisionTangentBasis、bUseFullPrecisionUVs和FFurSkinData的HasExtraBoneInfluences
	 */
	bUseHighPrecisionTangentBasis = TangentBasisTypeT == EStaticMeshVertexTangentBasisType::HighPrecision;
	bUseFullPrecisionUVs = UVTypeT == EStaticMeshVertexUVType::HighPrecision;
	HasExtraBoneInfluences = bExtraBoneInfluencesT;

	/*
	 * 从传入的LodRenderData获得Static Vertex的Position Buffer、蒙皮权重Buffer、Mesh顶点Buffer、顶点色Buffer
	 */
	const auto& SourcePositions = LodRenderData.StaticVertexBuffers.PositionVertexBuffer;
	const auto& SourceSkinWeights = LodRenderData.SkinWeightVertexBuffer;
	const auto& SourceVertices = LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer;
	const auto& SourceColors = LodRenderData.StaticVertexBuffers.ColorVertexBuffer;

	const uint32 SourceVertexCount = SourcePositions.GetNumVertices();
	check(SourceVertexCount == SourceSkinWeights.GetNumVertices() && SourceVertexCount == SourceVertices.GetNumVertices());

	bool HasVertexColor = SourceColors.GetNumVertices() > 0;
	check(!HasVertexColor || SourceVertexCount == SourceColors.GetNumVertices());

	if (Build == BuildType::Full)
	{
		/*
		 * 从传入的Mesh顶点buff中，将内存数据块解包，更新临时的成员变量Normals
		 */
		UnpackNormals<TangentBasisTypeT>(SourceVertices);
	}
	if (Build >= BuildType::Splines)
	{
		/*
		 * 从传入的Position Buffer，更新临时的成员变量SplineMap和VertexRemap
		 */
		GenerateSplineMap(SourcePositions);
	}

	uint32 NewVertexCount = VertexCountPerLayer * FurLayerCount;//得到总的顶点数

	/*
	 * 当上一帧的数据还没提交到渲染线程，就while在等待
	 */
	while (RenderThreadDataSubmissionPending)
		;

	TArray<FSection>& LocalSections = Sections.Num() ? TempSections : Sections;
	LocalSections.SetNum(LodRenderData.RenderSections.Num());

	/*
	 * 将Position Buffer、蒙皮权重Buffer、Mesh顶点Buffer、顶点色Buffer传递给VertexBlitter类（顶点位块，方便传输）
	 * 以便后续在GenerateFurVertices将CPU端的数据传递给GPU端
	 */
	FFurSkinVertexBlitter<TangentBasisTypeT, UVTypeT, bExtraBoneInfluencesT> VertexBlitter(SourcePositions, SourceVertices, SourceColors, SourceSkinWeights);

	/*
	 * 成员变量VertexBuffer锁定，大小为前面计算的顶点总数
	 * VertexBuffer在后续的FFurSceneProxy的构造函数中，会调用到FFurSkinData::CreateVertexFactories，为每个lod创建顶点工厂
	 */
	VertexType* Vertices = VertexBuffer.Lock<VertexType>(NewVertexCount);
	if (Vertices == nullptr)
	{
		return;
	}

	uint32 SectionVertexOffset = 0;//Section的顶点在顶点buffer中的偏移
	float MaxDistSq = 0.0f;
	for (int32 SectionIndex = 0; SectionIndex < LodRenderData.RenderSections.Num(); SectionIndex++)
	{
		/*
		 * 遍历传入LodRenderData的每个Section（FSkelMeshRenderSection结构体）
		 */
		const auto& SourceSection = LodRenderData.RenderSections[SectionIndex];
		FSection& FurSection = LocalSections[SectionIndex];

		FurSection.MinVertexIndex = SectionVertexOffset;

		/*
		 * 为当前Section调用模板函数GenerateFurVertices<T1,T2>()
		 * 生成顶点，其中T1是顶点类型，T2用于传输的位块类型
		 * 得到生成的顶点数VertCount
		 */
		uint32 VertCount = GenerateFurVertices(SourceSection.BaseVertexIndex
			, SourceSection.BaseVertexIndex + SourceSection.NumVertices
			, Vertices + SectionVertexOffset, VertexBlitter);
		
		if (Build == BuildType::Full)
		{
			/*
			 * 遍历上述生成的顶点（Vertices数组，索引需要加上偏移SectionVertexOffset，个数为VertCount）
			 * 寻找与骨骼参考点相距最大的距离MaxDistSq
			 * 最终保存为成员变量MaxVertexBoneDistance
			 */
			const auto& RefPose = SkeletalMesh->GetRefSkeleton().GetRawRefBonePose();
			for (uint32 i = 0; i < VertCount; i++)
			{
				uint32 VertexIndex = SectionVertexOffset + i;
				for (uint32 b = 0; b < VertexType::NumInfluences; b++)
				{
					if (Vertices[VertexIndex].InfluenceWeights[b] == 0)
						break;
					uint32 BoneIndex = SourceSection.BoneMap[Vertices[VertexIndex].InfluenceBones[b]];
					float distSq = FVector::DistSquared(FVector(Vertices[VertexIndex].Position), RefPose[BoneIndex].GetTranslation());
					if (distSq > MaxDistSq)
						MaxDistSq = distSq;
				}
			}
		}

		SectionVertexOffset += VertCount * FurLayerCount;//在顶点buffer中偏移这个Section的顶点的
		FurSection.MaxVertexIndex = SectionVertexOffset - 1;//记录这个Section的中最后一个顶点（在顶点buffer中）的索引
	}
	VertexBuffer.Unlock();//成员变量VertexBuffer解锁

	if (Build == BuildType::Full)
		MaxVertexBoneDistance = sqrtf(MaxDistSq);

	if (Build >= BuildType::Splines || FurLayerCount != OldFurLayerCount || RemoveFacesWithoutSplines != OldRemoveFacesWithoutSplines)
	{
		/*
		 * 基本是必进，Build都大于等于BuildType::Splines，没有Minimal的调用
		 */
		OldFurLayerCount = FurLayerCount;
		OldRemoveFacesWithoutSplines = RemoveFacesWithoutSplines;

		/*
		 * 从LodRenderData中获取SourceIndices
		 */
		TArray<uint32> SourceIndices;
		LodRenderData.MultiSizeIndexContainer.GetIndexBuffer(SourceIndices);

		/*
		 * 成员变量IndexBuffer锁定，大小为每层索引数（来自SourceIndices）乘以层数
		 * FFurData::GetIndexBuffer_RenderThread，可以在渲染线程获得IndexBuffer
		 */
		auto& Indices = IndexBuffer.Lock();
		Indices.Reset();
		Indices.AddUninitialized(SourceIndices.Num() * FurLayerCount);
		uint32 Idx = 0;
		for (int32 SectionIndex = 0; SectionIndex < LodRenderData.RenderSections.Num(); SectionIndex++)
		{
			/*
			 * 遍历Section
			 */
			const auto& SourceSection = LodRenderData.RenderSections[SectionIndex];
			FSection& FurSection = LocalSections[SectionIndex];

			FurSection.MaterialIndex = SourceSection.MaterialIndex;
			FurSection.BaseIndex = Idx;

			for (int32 Layer = 0; Layer < FurLayerCount; Layer++)
			{
				/*
				 * 遍历Fur每一层
				 */
				int32 VertexIndexOffset = Layer * ((FurSection.MaxVertexIndex - FurSection.MinVertexIndex + 1) / FurLayerCount) + FurSection.MinVertexIndex;
				check(VertexIndexOffset >= 0);
				if (FurSplinesUsed && RemoveFacesWithoutSplines)
				{
					/*
					 * 使用了引导网格，Idx的计算需要特殊
					 */
					for (uint32 t = 0; t < SourceSection.NumTriangles; ++t)
					{
						uint32 Idx0 = SourceIndices[SourceSection.BaseIndex + t * 3];
						uint32 Idx1 = SourceIndices[SourceSection.BaseIndex + t * 3 + 1];
						uint32 Idx2 = SourceIndices[SourceSection.BaseIndex + t * 3 + 2];
						if (SplineMap[Idx0] >= 0 && SplineMap[Idx1] >= 0 && SplineMap[Idx2] >= 0)
						{
							Indices[Idx++] = VertexRemap[Idx0] + VertexIndexOffset;
							Indices[Idx++] = VertexRemap[Idx1] + VertexIndexOffset;
							Indices[Idx++] = VertexRemap[Idx2] + VertexIndexOffset;
						}
					}
				}
				else
				{
					VertexIndexOffset -= SourceSection.BaseVertexIndex;
					for (uint32 i = 0; i < SourceSection.NumTriangles * 3; ++i)
						Indices[Idx++] = SourceIndices[SourceSection.BaseIndex + i] + VertexIndexOffset;
				}
			}
			FurSection.NumTriangles = (Idx - FurSection.BaseIndex) / 3;
			FurSection.NumBones = SourceSection.BoneMap.Num();
		}
		check(Idx <= (uint32)Indices.Num());
		Indices.RemoveAt(Idx, Indices.Num() - Idx, false);
		IndexBuffer.Unlock();//成员变量IndexBuffer解锁

		if (TempSections.Num())
		{
			/*
			 * 转入渲染线程，将在Game线程更新的TempSections，更新渲染线程的Section
			 */
			ENQUEUE_RENDER_COMMAND(UpdateDataCommand)([this, NewVertexCount](FRHICommandListImmediate& RHICmdList) {
				Sections = TempSections;
				VertexCount = NewVertexCount;
			});
		}
		else
		{
			VertexCount = NewVertexCount;
		}
	}

	/*
	 * RenderThreadDataSubmissionPending设置为true，阻塞再次进入Sections刷新的流程
	 * 直到渲染线程的命令队列执行新的命令，意味着之前的Section更新命令已经完成
	 * RenderThreadDataSubmissionPending才会设置为false，开放Sections刷新流程
	 */
	RenderThreadDataSubmissionPending = true;
	ENQUEUE_RENDER_COMMAND(UpdateDataCommand)([this](FRHICommandListImmediate& RHICmdList) {
		RenderThreadDataSubmissionPending = false;
	});

#if !WITH_EDITORONLY_DATA
	Normals.SetNum(0, true);
	SplineMap.SetNum(0, true);
	VertexRemap.SetNum(0, true);
#endif // WITH_EDITORONLY_DATA
}

void FFurSkinData::BuildFur(const TArray<uint32>& InVertexSet)
{
	auto* SkeletalMeshResource = SkeletalMesh->GetResourceForRendering();
	check(SkeletalMeshResource);

	const FSkeletalMeshLODRenderData& LodRenderData = SkeletalMeshResource->LODRenderData[Lod];
	if (LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetUseHighPrecisionTangentBasis())
		BuildFur<EStaticMeshVertexTangentBasisType::HighPrecision>(LodRenderData, InVertexSet);
	else
		BuildFur<EStaticMeshVertexTangentBasisType::Default>(LodRenderData, InVertexSet);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, const TArray<uint32>& InVertexSet)
{
	if (LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetUseFullPrecisionUVs())
		BuildFur<TangentBasisTypeT, EStaticMeshVertexUVType::HighPrecision>(LodRenderData, InVertexSet);
	else
		BuildFur<TangentBasisTypeT, EStaticMeshVertexUVType::Default>(LodRenderData, InVertexSet);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, const TArray<uint32>& InVertexSet)
{
	if (LodRenderData.SkinWeightVertexBuffer.GetMaxBoneInfluences() > 4)
		BuildFur<TangentBasisTypeT, UVTypeT, true>(LodRenderData, InVertexSet);
	else
		BuildFur<TangentBasisTypeT, UVTypeT, false>(LodRenderData, InVertexSet);
}

template<EStaticMeshVertexTangentBasisType TangentBasisTypeT, EStaticMeshVertexUVType UVTypeT, bool bExtraBoneInfluencesT>
inline void FFurSkinData::BuildFur(const FSkeletalMeshLODRenderData& LodRenderData, const TArray<uint32>& InVertexSet)
{
	typedef FFurSkinVertex<TangentBasisTypeT, UVTypeT, bExtraBoneInfluencesT> VertexType;

	while (RenderThreadDataSubmissionPending)
		;

	const auto& SrcSections = LodRenderData.RenderSections;
	uint32 SectionIndex = 0;
	uint32 SectionCount = SrcSections.Num();
	uint32 SectionVertexIndexBegin = SrcSections[SectionIndex].BaseVertexIndex;
	uint32 SectionVertexIndexEnd = SectionVertexIndexBegin + SrcSections[SectionIndex].NumVertices;

	const auto& LocalSections = TempSections.Num() ? TempSections : Sections;
	uint32 DstSectionVertexBegin = LocalSections[SectionIndex].MinVertexIndex;
	uint32 DstSectionVertexCountPerLayer = (LocalSections[SectionIndex].MaxVertexIndex + 1 - DstSectionVertexBegin) / FurLayerCount;

	TArray<float> FurLengths;
	GenerateFurLengths(FurLengths);

	VertexType* Vertices = VertexBuffer.Lock<VertexType>(VertexCountPerLayer * FurLayerCount);
	bool UseRemap = VertexRemap.Num() > 0;
	for (int32 Layer = 0; Layer < FurLayerCount; Layer++)
	{
		auto GenLayerData = CalcFurGenLayerData(FurLayerCount - Layer);
		for (uint32 SrcVertexIndex : InVertexSet)
		{
			uint32 checkCounter = 0;
			while (SrcVertexIndex < SectionVertexIndexBegin || SrcVertexIndex >= SectionVertexIndexEnd)
			{
				SectionIndex = (SectionIndex + 1) % SectionCount;
				SectionVertexIndexBegin = SrcSections[SectionIndex].BaseVertexIndex;
				SectionVertexIndexEnd = SrcSections[SectionIndex].BaseVertexIndex + SrcSections[SectionIndex].NumVertices;
				DstSectionVertexBegin = LocalSections[SectionIndex].MinVertexIndex;
				DstSectionVertexCountPerLayer = (LocalSections[SectionIndex].MaxVertexIndex + 1 - DstSectionVertexBegin) / FurLayerCount;
				check(checkCounter++ < SectionCount);
			}
			uint32 DstVertexIndex = UseRemap ? VertexRemap[SrcVertexIndex] : SrcVertexIndex - SectionVertexIndexBegin;
			DstVertexIndex += DstSectionVertexCountPerLayer * Layer + DstSectionVertexBegin;
			VertexType& Vertex = Vertices[DstVertexIndex];

			if (FurSplinesUsed)
			{
				int32 SplineIndex = SplineMap[SrcVertexIndex];
				float Length = SplineIndex >= 0 ? FurLengths[SplineIndex] : FurLength;
				GenerateFurVertex(Vertex.FurOffset, Vertex.UV1, Vertex.UV2, Vertex.UV3, FVector3f(Normals[SrcVertexIndex]), Length, GenLayerData, SplineIndex);
			}
			else
			{
				GenerateFurVertex(Vertex.FurOffset, Vertex.UV1, Vertex.UV2, Vertex.UV3, FVector3f(Normals[SrcVertexIndex]), FurLength, GenLayerData);
			}
		}
	}

	VertexBuffer.Unlock();

	RenderThreadDataSubmissionPending = true;
	ENQUEUE_RENDER_COMMAND(UpdateDataCommand)([this](FRHICommandListImmediate& RHICmdList) {
		RenderThreadDataSubmissionPending = false;
	});
}

/** Generate Splines */
void GenerateSplines(UFurSplines* Splines, USkeletalMesh* InSkeletalMesh, int32 InLod, const TArray<USkeletalMesh*>& InGuideMeshes)
{
	/*
	 * 从传入的SkeletalMesh获得指定Lod下的RenderData
	 * 进而获得其Static Vertex的Position Buffer
	 */
	auto* SkeletalMeshResource = InSkeletalMesh->GetResourceForRendering();
	check(SkeletalMeshResource);
	if (InLod >= SkeletalMeshResource->LODRenderData.Num())
		InLod = SkeletalMeshResource->LODRenderData.Num() - 1;
	const auto& LodModel = SkeletalMeshResource->LODRenderData[InLod];
	const auto& SourcePositions = LodModel.StaticVertexBuffers.PositionVertexBuffer;

	/*
	 * 遍历SourcePositions，将顶点位置信息设置给Splines的Vertices
	 * Splines的Vertices的索引要考虑预留引导网格的顶点，通过ControlPointCount控制
	 */
	uint32 VertexCount = SourcePositions.GetNumVertices();
	int32 ControlPointCount = InGuideMeshes.Num() + 1;
	Splines->Vertices.AddUninitialized(VertexCount * ControlPointCount);
	Splines->ControlPointCount = ControlPointCount;
	for (uint32 i = 0; i < VertexCount; i++)
	{
		int32 Index = i * ControlPointCount;
		Splines->Vertices[Index] = FVector(SourcePositions.VertexPosition(i));
	}

	/*
	 * 遍历传入的GuideMeshes数组（数组的每个元素将成为control point）
	 * 类似地获取相应的Position Buffer，设置给Splines的Vertices
	 * 索引计算考虑引导网格数组索引k（可以理解为控制点层数）
	 *
	 * 通过这步处理，将（当前lod下）Splines的Vertices填充了一层基础层（以传入InSkeletalMesh为准）
	 * + n层引导层（InGuideMeshes数组，每个网格为一层，每层顶点数为当前层引导网格和基础层网格顶点数中少的那个）
	 */
	int32 k = 1;
	for (USkeletalMesh* GuideMesh : InGuideMeshes)
	{
		if (GuideMesh)
		{
			auto* SkeletalMeshResource2 = GuideMesh->GetResourceForRendering();
			check(SkeletalMeshResource2);
			const auto& LodModel2 = SkeletalMeshResource2->LODRenderData[InLod];
			const auto& SourcePositions2 = LodModel2.StaticVertexBuffers.PositionVertexBuffer;
			int32 c = FMath::Min(SourcePositions2.GetNumVertices(), VertexCount);
			for (int32 i = 0; i < c; i++)
				Splines->Vertices[i * ControlPointCount + k] = FVector(SourcePositions2.VertexPosition(i));
			for (uint32 i = c; i < VertexCount; i++)
				Splines->Vertices[i * ControlPointCount + k] = Splines->Vertices[i * ControlPointCount + (k - 1)];
		}
		else
		{
			for (uint32 i = 0; i < VertexCount; i++)
				Splines->Vertices[i * ControlPointCount + k] = Splines->Vertices[i * ControlPointCount + (k - 1)];
		}
		k++;
	}
}
