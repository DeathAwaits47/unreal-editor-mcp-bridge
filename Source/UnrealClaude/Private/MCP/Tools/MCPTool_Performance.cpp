// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Performance.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"

namespace
{
	template<typename T>
	T* LoadGameAsset(const FString& InPath)
	{
		FString ObjectPath = InPath;
		T* Asset = LoadObject<T>(nullptr, *ObjectPath);
		if (!Asset && InPath.StartsWith(TEXT("/Game/")) && !InPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(InPath);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *InPath, *AssetName);
			Asset = LoadObject<T>(nullptr, *ObjectPath);
		}
		return Asset;
	}

	TArray<TSharedPtr<FJsonValue>> BuildTopAssetArray(const TMap<FString, int64>& TrianglesByMesh, const TMap<FString, int32>& ReferencesByMesh, int32 MaxResults)
	{
		TArray<TPair<FString, int64>> Sorted;
		for (const TPair<FString, int64>& Pair : TrianglesByMesh)
		{
			Sorted.Add(Pair);
		}
		Sorted.Sort([](const TPair<FString, int64>& A, const TPair<FString, int64>& B) { return A.Value > B.Value; });

		TArray<TSharedPtr<FJsonValue>> Result;
		for (int32 Index = 0; Index < FMath::Min(MaxResults, Sorted.Num()); ++Index)
		{
			const TPair<FString, int64>& Pair = Sorted[Index];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("mesh"), Pair.Key);
			Item->SetNumberField(TEXT("triangles_per_mesh"), static_cast<double>(Pair.Value));
			Item->SetNumberField(TEXT("component_references"), ReferencesByMesh.FindRef(Pair.Key));
			Item->SetNumberField(TEXT("estimated_total_triangles"), static_cast<double>(Pair.Value * ReferencesByMesh.FindRef(Pair.Key)));
			Result.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Result;
	}

	FString BlendModeToText(EBlendMode BlendMode)
	{
		switch (BlendMode)
		{
		case BLEND_Opaque: return TEXT("Opaque");
		case BLEND_Masked: return TEXT("Masked");
		case BLEND_Translucent: return TEXT("Translucent");
		case BLEND_Additive: return TEXT("Additive");
		case BLEND_Modulate: return TEXT("Modulate");
		case BLEND_AlphaComposite: return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout: return TEXT("AlphaHoldout");
		default: return TEXT("Unknown");
		}
	}
}

FMCPToolInfo FMCPTool_Performance::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("performance");
	Info.Description = TEXT(
		"Audit performance and material/shader risk in the active UE level. scene_audit reports actors, primitive components, triangle estimates, instancing, shadow casters, movable lights, Nanite usage, and heavily referenced assets. "
		"material_audit reports a material's blend mode, two-sided state, expression categories, and likely expensive features. "
		"runtime_profile_command only issues an explicit UE console profile command; use it during PIE or a packaged build for actual frame-time evidence.");
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("scene_audit, material_audit, or runtime_profile_command"), true),
		FMCPToolParameter(TEXT("material_path"), TEXT("string"), TEXT("Material or material-instance asset path; required for material_audit"), false),
		FMCPToolParameter(TEXT("top_results"), TEXT("number"), TEXT("Top costly mesh assets to return; default 15, max 100"), false),
		FMCPToolParameter(TEXT("runtime_command"), TEXT("string"), TEXT("stat_unit, stat_gpu, stat_rhi, start_trace, or stop_trace; required for runtime_profile_command"), false)
	};
	Info.Annotations = FMCPToolAnnotations::Modifying(); // runtime_profile_command executes an explicit console command.
	return Info;
}

FMCPToolResult FMCPTool_Performance::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, ParamError)) return ParamError.GetValue();
	if (Operation.Equals(TEXT("scene_audit"), ESearchCase::IgnoreCase)) return ExecuteSceneAudit(Params);
	if (Operation.Equals(TEXT("material_audit"), ESearchCase::IgnoreCase)) return ExecuteMaterialAudit(Params);
	if (Operation.Equals(TEXT("runtime_profile_command"), ESearchCase::IgnoreCase)) return ExecuteRuntimeProfileCommand(Params);
	return FMCPToolResult::Error(FString::Printf(TEXT("Unknown performance operation '%s'. Valid: scene_audit, material_audit, runtime_profile_command"), *Operation));
}

FMCPToolResult FMCPTool_Performance::ExecuteSceneAudit(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();

	const int32 MaxResults = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("top_results"), 15), 1, 100);
	int32 ActorCount = 0;
	int32 PrimitiveCount = 0;
	int32 StaticMeshComponents = 0;
	int32 SkeletalMeshComponents = 0;
	int32 InstancedComponents = 0;
	int64 InstancedMeshInstances = 0;
	int32 ShadowCastingPrimitives = 0;
	int32 MovablePrimitives = 0;
	int32 TotalLights = 0;
	int32 MovableLights = 0;
	int32 ShadowCastingLights = 0;
	int32 NaniteStaticMeshComponents = 0;
	int64 EstimatedStaticTriangles = 0;
	TSet<FString> UniqueMaterials;
	TMap<FString, int64> TrianglesByMesh;
	TMap<FString, int32> ReferencesByMesh;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor->IsTemplate()) continue;
		++ActorCount;

		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive) || Primitive->IsTemplate()) continue;
			++PrimitiveCount;
			if (Primitive->CastShadow) ++ShadowCastingPrimitives;
			if (Primitive->Mobility == EComponentMobility::Movable) ++MovablePrimitives;

			for (int32 MaterialIndex = 0; MaterialIndex < Primitive->GetNumMaterials(); ++MaterialIndex)
			{
				if (UMaterialInterface* Material = Primitive->GetMaterial(MaterialIndex))
				{
					UniqueMaterials.Add(Material->GetPathName());
				}
			}

			if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Primitive))
			{
				++StaticMeshComponents;
				if (UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh())
				{
					const FString MeshPath = StaticMesh->GetPathName();
					const int64 TriangleCount = StaticMesh->GetNumTriangles(0);
					EstimatedStaticTriangles += TriangleCount;
					TrianglesByMesh.FindOrAdd(MeshPath) = TriangleCount;
					ReferencesByMesh.FindOrAdd(MeshPath)++;
					if (StaticMesh->IsNaniteEnabled()) ++NaniteStaticMeshComponents;
				}
			}

			if (Cast<USkeletalMeshComponent>(Primitive))
			{
				++SkeletalMeshComponents;
			}

			if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(Primitive))
			{
				++InstancedComponents;
				InstancedMeshInstances += InstancedComponent->GetInstanceCount();
			}
		}

		TInlineComponentArray<ULightComponent*> Lights(Actor);
		for (ULightComponent* Light : Lights)
		{
			if (!IsValid(Light) || Light->IsTemplate()) continue;
			++TotalLights;
			if (Light->Mobility == EComponentMobility::Movable) ++MovableLights;
			if (Light->CastShadows) ++ShadowCastingLights;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world"), World->GetPathName());
	Data->SetNumberField(TEXT("actors"), ActorCount);
	Data->SetNumberField(TEXT("primitive_components"), PrimitiveCount);
	Data->SetNumberField(TEXT("static_mesh_components"), StaticMeshComponents);
	Data->SetNumberField(TEXT("skeletal_mesh_components"), SkeletalMeshComponents);
	Data->SetNumberField(TEXT("instanced_components"), InstancedComponents);
	Data->SetNumberField(TEXT("instanced_mesh_instances"), static_cast<double>(InstancedMeshInstances));
	Data->SetNumberField(TEXT("estimated_static_mesh_triangles"), static_cast<double>(EstimatedStaticTriangles));
	Data->SetNumberField(TEXT("shadow_casting_primitives"), ShadowCastingPrimitives);
	Data->SetNumberField(TEXT("movable_primitives"), MovablePrimitives);
	Data->SetNumberField(TEXT("lights"), TotalLights);
	Data->SetNumberField(TEXT("movable_lights"), MovableLights);
	Data->SetNumberField(TEXT("shadow_casting_lights"), ShadowCastingLights);
	Data->SetNumberField(TEXT("nanite_static_mesh_components"), NaniteStaticMeshComponents);
	Data->SetNumberField(TEXT("unique_materials_referenced"), UniqueMaterials.Num());
	Data->SetArrayField(TEXT("top_static_meshes_by_estimated_triangles"), BuildTopAssetArray(TrianglesByMesh, ReferencesByMesh, MaxResults));

	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Completed editor-level scene performance audit"), Data);
	if (ShadowCastingPrimitives > 1000) Result.Warnings.Add(FString::Printf(TEXT("%d primitive components cast shadows. Audit foliage/PCG and small clutter before disabling important hero shadows."), ShadowCastingPrimitives));
	if (MovableLights > 4) Result.Warnings.Add(FString::Printf(TEXT("%d movable lights found. Test them with stat gpu in the actual gameplay camera; shadowed movable lights are usually the first lighting cost to validate."), MovableLights));
	if (InstancedMeshInstances == 0 && StaticMeshComponents > 300) Result.Warnings.Add(TEXT("No instanced static-mesh instances were found while the level has many static mesh components. Repeated small props/foliage may benefit from existing Foliage Types, PCG, or HISM placement after profiling."));
	Result.Warnings.Add(TEXT("Triangle and component counts are editor-side indicators, not a frame-time verdict. Use runtime_profile_command in PIE or a packaged build for stat unit/stat gpu evidence."));
	return Result;
}

FMCPToolResult FMCPTool_Performance::ExecuteMaterialAudit(const TSharedRef<FJsonObject>& Params)
{
	FString MaterialPath;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("material_path"), MaterialPath, ParamError)) return ParamError.GetValue();
	UMaterialInterface* Material = LoadGameAsset<UMaterialInterface>(MaterialPath);
	if (!Material) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load material '%s'"), *MaterialPath));

	UMaterial* BaseMaterial = Material->GetBaseMaterial();
	if (!BaseMaterial) return FMCPToolResult::Error(TEXT("Material has no base material."));

	int32 ExpressionCount = 0;
	int32 TextureSampleExpressions = 0;
	int32 SceneTextureExpressions = 0;
	int32 CustomExpressions = 0;
	int32 WorldPositionExpressions = 0;
	int32 RuntimeVirtualTextureExpressions = 0;
	TMap<FString, int32> ExpressionClasses;
#if WITH_EDITORONLY_DATA
	for (UMaterialExpression* Expression : BaseMaterial->GetExpressions())
	{
		if (!Expression) continue;
		++ExpressionCount;
		const FString ClassName = Expression->GetClass()->GetName();
		ExpressionClasses.FindOrAdd(ClassName)++;
		if (ClassName.Contains(TEXT("TextureSample"))) ++TextureSampleExpressions;
		if (ClassName.Contains(TEXT("SceneTexture"))) ++SceneTextureExpressions;
		if (ClassName.Contains(TEXT("Custom"))) ++CustomExpressions;
		if (ClassName.Contains(TEXT("WorldPosition"))) ++WorldPositionExpressions;
		if (ClassName.Contains(TEXT("RuntimeVirtualTexture"))) ++RuntimeVirtualTextureExpressions;
	}
#endif

	TArray<TPair<FString, int32>> SortedClasses;
	for (const TPair<FString, int32>& Pair : ExpressionClasses) SortedClasses.Add(Pair);
	SortedClasses.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
	TArray<TSharedPtr<FJsonValue>> ClassSummary;
	for (int32 Index = 0; Index < FMath::Min(20, SortedClasses.Num()); ++Index)
	{
		TSharedPtr<FJsonObject> ClassInfo = MakeShared<FJsonObject>();
		ClassInfo->SetStringField(TEXT("expression_class"), SortedClasses[Index].Key);
		ClassInfo->SetNumberField(TEXT("count"), SortedClasses[Index].Value);
		ClassSummary.Add(MakeShared<FJsonValueObject>(ClassInfo));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), Material->GetPathName());
	Data->SetStringField(TEXT("base_material"), BaseMaterial->GetPathName());
	Data->SetStringField(TEXT("blend_mode"), BlendModeToText(BaseMaterial->GetBlendMode()));
	Data->SetBoolField(TEXT("two_sided"), BaseMaterial->IsTwoSided());
	Data->SetBoolField(TEXT("masked"), BaseMaterial->IsMasked());
	Data->SetBoolField(TEXT("dithered_lod_transition"), BaseMaterial->IsDitheredLODTransition());
	Data->SetNumberField(TEXT("expression_count"), ExpressionCount);
	Data->SetNumberField(TEXT("texture_sample_expressions"), TextureSampleExpressions);
	Data->SetNumberField(TEXT("scene_texture_expressions"), SceneTextureExpressions);
	Data->SetNumberField(TEXT("custom_expressions"), CustomExpressions);
	Data->SetNumberField(TEXT("world_position_expressions"), WorldPositionExpressions);
	Data->SetNumberField(TEXT("runtime_virtual_texture_expressions"), RuntimeVirtualTextureExpressions);
	Data->SetArrayField(TEXT("expression_class_summary"), ClassSummary);

	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Completed material/shader risk audit"), Data);
	if (BaseMaterial->GetBlendMode() == BLEND_Translucent || BaseMaterial->GetBlendMode() == BLEND_Additive || BaseMaterial->GetBlendMode() == BLEND_AlphaComposite)
		Result.Warnings.Add(TEXT("This is a translucent-style material. Validate it with stat gpu in the gameplay camera, particularly when layered over foliage, fog, or particles."));
	if (BaseMaterial->IsTwoSided()) Result.Warnings.Add(TEXT("Two-sided rendering doubles the raster-facing work for many assets. It is normal for foliage/cards, but should be intentional on hero geometry."));
	if (TextureSampleExpressions > 16) Result.Warnings.Add(FString::Printf(TEXT("%d texture-sample expression nodes found. This is not automatically wrong, but it is a strong candidate for Material Editor Stats and shader-complexity inspection."), TextureSampleExpressions));
	if (SceneTextureExpressions > 0 || CustomExpressions > 0) Result.Warnings.Add(TEXT("SceneTexture or Custom material expressions found. Keep these out of broad foliage/terrain coverage unless a runtime profile proves them affordable."));
	return Result;
}

FMCPToolResult FMCPTool_Performance::ExecuteRuntimeProfileCommand(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString RequestedCommand;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("runtime_command"), RequestedCommand, ParamError)) return ParamError.GetValue();

	FString ConsoleCommand;
	if (RequestedCommand.Equals(TEXT("stat_unit"), ESearchCase::IgnoreCase)) ConsoleCommand = TEXT("stat unit");
	else if (RequestedCommand.Equals(TEXT("stat_gpu"), ESearchCase::IgnoreCase)) ConsoleCommand = TEXT("stat gpu");
	else if (RequestedCommand.Equals(TEXT("stat_rhi"), ESearchCase::IgnoreCase)) ConsoleCommand = TEXT("stat rhi");
	else if (RequestedCommand.Equals(TEXT("start_trace"), ESearchCase::IgnoreCase)) ConsoleCommand = TEXT("trace.start cpu,frame,gpu,bookmark");
	else if (RequestedCommand.Equals(TEXT("stop_trace"), ESearchCase::IgnoreCase)) ConsoleCommand = TEXT("trace.stop");
	else return FMCPToolResult::Error(TEXT("runtime_command must be stat_unit, stat_gpu, stat_rhi, start_trace, or stop_trace."));

	if (!GEngine) return FMCPToolResult::Error(TEXT("Engine is unavailable."));
	const bool bAccepted = GEngine->Exec(World, *ConsoleCommand);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("console_command"), ConsoleCommand);
	Data->SetBoolField(TEXT("engine_accepted_command"), bAccepted);
	Data->SetBoolField(TEXT("is_game_world"), World->IsGameWorld());
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Issued runtime profiling command"), Data);
	if (!World->IsGameWorld()) Result.Warnings.Add(TEXT("This is not currently a PIE/game world. Start PIE or profile a packaged build, then issue the command again for useful timing data."));
	if (RequestedCommand.Equals(TEXT("start_trace"), ESearchCase::IgnoreCase)) Result.Warnings.Add(TEXT("Run gameplay through the expensive scene before stop_trace, then open the generated trace in Unreal Insights."));
	return Result;
}
