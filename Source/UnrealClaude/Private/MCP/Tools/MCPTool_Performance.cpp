// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Performance.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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

FMCPTool_Performance::~FMCPTool_Performance()
{
	StopPIECapture(false);
}

FMCPToolInfo FMCPTool_Performance::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("performance");
	Info.Description = TEXT(
		"Audit performance and material/shader risk in the active UE level. scene_audit reports actors, primitive components, triangle estimates, instancing, shadow casters, movable lights, Nanite usage, and heavily referenced assets. "
		"material_audit reports a material's blend mode, two-sided state, expression categories, and likely expensive features. "
		"pie_capture records CPU, render, GPU and frame-time samples continuously while PIE is running, then writes a JSON report under Saved/MCPPerformanceCaptures. "
		"runtime_profile_command only issues an explicit UE console profile command; use it during PIE or a packaged build for deeper frame-time evidence.");
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("scene_audit, material_audit, runtime_profile_command, or pie_capture"), true),
		FMCPToolParameter(TEXT("material_path"), TEXT("string"), TEXT("Material or material-instance asset path; required for material_audit"), false),
		FMCPToolParameter(TEXT("top_results"), TEXT("number"), TEXT("Top costly mesh assets to return; default 15, max 100"), false),
		FMCPToolParameter(TEXT("runtime_command"), TEXT("string"), TEXT("stat_unit, stat_gpu, stat_rhi, start_trace, or stop_trace; required for runtime_profile_command"), false),
		FMCPToolParameter(TEXT("capture_action"), TEXT("string"), TEXT("start, stop, or status; required for pie_capture"), false),
		FMCPToolParameter(TEXT("sample_interval_seconds"), TEXT("number"), TEXT("PIE sample interval, 0.05 to 1.0 seconds; default 0.25"), false),
		FMCPToolParameter(TEXT("max_duration_seconds"), TEXT("number"), TEXT("Automatic capture-stop duration, 5 to 900 seconds; default 180"), false)
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
	if (Operation.Equals(TEXT("pie_capture"), ESearchCase::IgnoreCase)) return ExecutePIECapture(Params);
	return FMCPToolResult::Error(FString::Printf(TEXT("Unknown performance operation '%s'. Valid: scene_audit, material_audit, runtime_profile_command, pie_capture"), *Operation));
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

FMCPToolResult FMCPTool_Performance::ExecutePIECapture(const TSharedRef<FJsonObject>& Params)
{
	const FString Action = ExtractOptionalString(Params, TEXT("capture_action"));
	if (Action.Equals(TEXT("start"), ESearchCase::IgnoreCase))
	{
		if (!GEditor || !GEditor->PlayWorld)
		{
			return FMCPToolResult::Error(TEXT("Start PIE first, then start the capture. The recorder only samples the actual gameplay world."));
		}
		if (PIECaptureTickerHandle.IsValid())
		{
			return FMCPToolResult::Error(TEXT("A PIE performance capture is already running. Use capture_action=status or stop."));
		}

		PIECaptureSamples.Reset();
		LastPIECaptureReportPath.Reset();
		PIECaptureIntervalSeconds = FMath::Clamp(ExtractOptionalNumber<double>(Params, TEXT("sample_interval_seconds"), 0.25), 0.05, 1.0);
		PIECaptureMaxDurationSeconds = FMath::Clamp(ExtractOptionalNumber<double>(Params, TEXT("max_duration_seconds"), 180.0), 5.0, 900.0);
		PIECaptureStartedAt = FPlatformTime::Seconds();
		PIECaptureLastSampleAt = 0.0;
		PIECaptureTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FMCPTool_Performance::TickPIECapture),
			0.0f);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("world"), GEditor->PlayWorld->GetPathName());
		Data->SetNumberField(TEXT("sample_interval_seconds"), PIECaptureIntervalSeconds);
		Data->SetNumberField(TEXT("max_duration_seconds"), PIECaptureMaxDurationSeconds);
		FMCPToolResult Result = FMCPToolResult::Success(TEXT("Started continuous PIE performance capture"), Data);
		Result.Warnings.Add(TEXT("Keep the PIE window focused and play through the area you want profiled. Call pie_capture/stop when done; a JSON report will be saved in Saved/MCPPerformanceCaptures."));
		return Result;
	}

	if (Action.Equals(TEXT("status"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> Data = BuildPIECaptureSummary();
		Data->SetBoolField(TEXT("capturing"), PIECaptureTickerHandle.IsValid());
		if (!LastPIECaptureReportPath.IsEmpty()) Data->SetStringField(TEXT("last_report"), LastPIECaptureReportPath);
		return FMCPToolResult::Success(TEXT("Read PIE performance capture status"), Data);
	}

	if (Action.Equals(TEXT("stop"), ESearchCase::IgnoreCase))
	{
		if (!PIECaptureTickerHandle.IsValid())
		{
			return FMCPToolResult::Error(TEXT("No active PIE performance capture to stop."));
		}
		StopPIECapture(true);
		TSharedPtr<FJsonObject> Data = BuildPIECaptureSummary();
		Data->SetBoolField(TEXT("capturing"), false);
		Data->SetStringField(TEXT("report_path"), LastPIECaptureReportPath);
		return FMCPToolResult::Success(TEXT("Stopped PIE capture and saved the report"), Data);
	}

	return FMCPToolResult::Error(TEXT("pie_capture requires capture_action=start, status, or stop."));
}

bool FMCPTool_Performance::TickPIECapture(float DeltaSeconds)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		StopPIECapture(true);
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	if (PIECaptureLastSampleAt <= 0.0 || Now - PIECaptureLastSampleAt >= PIECaptureIntervalSeconds)
	{
		FPIEPerfSample& Sample = PIECaptureSamples.AddDefaulted_GetRef();
		Sample.ElapsedSeconds = Now - PIECaptureStartedAt;
		Sample.FrameMs = FApp::GetDeltaTime() * 1000.0;
		Sample.GameThreadMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
		Sample.RenderThreadMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
		Sample.GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
		PIECaptureLastSampleAt = Now;
	}

	if (Now - PIECaptureStartedAt >= PIECaptureMaxDurationSeconds)
	{
		StopPIECapture(true);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FMCPTool_Performance::BuildPIECaptureSummary() const
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("samples"), PIECaptureSamples.Num());
	if (PIECaptureSamples.IsEmpty()) return Summary;

	double FrameSum = 0.0, GameSum = 0.0, RenderSum = 0.0, GpuSum = 0.0;
	double MaxFrame = 0.0, MaxGame = 0.0, MaxRender = 0.0, MaxGpu = 0.0;
	int32 Hitches = 0;
	for (const FPIEPerfSample& Sample : PIECaptureSamples)
	{
		FrameSum += Sample.FrameMs; GameSum += Sample.GameThreadMs; RenderSum += Sample.RenderThreadMs; GpuSum += Sample.GpuMs;
		MaxFrame = FMath::Max(MaxFrame, Sample.FrameMs); MaxGame = FMath::Max(MaxGame, Sample.GameThreadMs);
		MaxRender = FMath::Max(MaxRender, Sample.RenderThreadMs); MaxGpu = FMath::Max(MaxGpu, Sample.GpuMs);
		if (Sample.FrameMs >= 33.3) ++Hitches;
	}
	const double Count = PIECaptureSamples.Num();
	const double AverageGame = GameSum / Count;
	const double AverageRender = RenderSum / Count;
	const double AverageGpu = GpuSum / Count;
	Summary->SetNumberField(TEXT("duration_seconds"), PIECaptureSamples.Last().ElapsedSeconds);
	Summary->SetNumberField(TEXT("average_frame_ms"), FrameSum / Count);
	Summary->SetNumberField(TEXT("average_game_thread_ms"), AverageGame);
	Summary->SetNumberField(TEXT("average_render_thread_ms"), AverageRender);
	Summary->SetNumberField(TEXT("average_gpu_ms"), AverageGpu);
	Summary->SetNumberField(TEXT("max_frame_ms"), MaxFrame);
	Summary->SetNumberField(TEXT("max_game_thread_ms"), MaxGame);
	Summary->SetNumberField(TEXT("max_render_thread_ms"), MaxRender);
	Summary->SetNumberField(TEXT("max_gpu_ms"), MaxGpu);
	Summary->SetNumberField(TEXT("hitch_samples_over_33ms"), Hitches);
	Summary->SetStringField(TEXT("likely_primary_limit"), AverageGpu > AverageGame + 0.5 && AverageGpu > AverageRender + 0.5 ? TEXT("GPU") : (AverageGame > AverageRender + 0.5 ? TEXT("GameThread") : TEXT("Mixed or render-thread limited")));
	return Summary;
}

FString FMCPTool_Performance::SavePIECaptureReport() const
{
	if (PIECaptureSamples.IsEmpty()) return FString();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("capture_type"), TEXT("PIE"));
	Root->SetStringField(TEXT("created_local_time"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Root->SetObjectField(TEXT("summary"), BuildPIECaptureSummary());
	TArray<TSharedPtr<FJsonValue>> SamplesJson;
	SamplesJson.Reserve(PIECaptureSamples.Num());
	for (const FPIEPerfSample& Sample : PIECaptureSamples)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("elapsed_seconds"), Sample.ElapsedSeconds);
		Item->SetNumberField(TEXT("frame_ms"), Sample.FrameMs);
		Item->SetNumberField(TEXT("game_thread_ms"), Sample.GameThreadMs);
		Item->SetNumberField(TEXT("render_thread_ms"), Sample.RenderThreadMs);
		Item->SetNumberField(TEXT("gpu_ms"), Sample.GpuMs);
		SamplesJson.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("samples"), SamplesJson);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer)) return FString();
	const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCPPerformanceCaptures"));
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString Path = FPaths::Combine(Folder, FString::Printf(TEXT("PIEPerformance_%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	return FFileHelper::SaveStringToFile(Json, *Path) ? Path : FString();
}

void FMCPTool_Performance::StopPIECapture(bool bWriteReport)
{
	if (PIECaptureTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PIECaptureTickerHandle);
		PIECaptureTickerHandle.Reset();
	}
	if (bWriteReport && !PIECaptureSamples.IsEmpty())
	{
		LastPIECaptureReportPath = SavePIECaptureReport();
	}
}
