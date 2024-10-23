// Copyright 2023 GiM s.r.o. All Rights Reserved.

#pragma once

#include "Runtime/Engine/Classes/Components/SkinnedMeshComponent.h"

class FFurSkinData;

/** Fur Morph Vertex Buffer */
class FFurMorphVertexBuffer : public FVertexBuffer
{
public:
	/**
	* Initialize the dynamic RHI for this rendering resource
	*/
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;

	/**
	* Release the dynamic RHI for this rendering resource
	*/
	virtual void ReleaseRHI() override;

	/**
	* Morph target vertex name
	*/
	virtual FString GetFriendlyName() const { return TEXT("gFur Morph target mesh vertices"); }

	/**
	 * Get Resource Size : mostly copied from InitDynamicRHI - how much they allocate when initialize
	 */

	int32 NumVertices;
};

class FFurMorphObject
{
public:
	FFurMorphObject(FFurSkinData* InFurData);
	~FFurMorphObject();

	void Update_RenderThread(FRHICommandListImmediate& RHICmdList, FMorphTargetWeightMap& ActiveMorphTargets, const TArray<float>& MorphTargetWeights, const TArray<TArray<int32>>& InMorphRemapTable, int InMeshLod);

	FVertexBuffer* GetVertexBuffer() { return &VertexBuffer; }

private:
	FFurSkinData* FurData;
	FFurMorphVertexBuffer VertexBuffer;
};
