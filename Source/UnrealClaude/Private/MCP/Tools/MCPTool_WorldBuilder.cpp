// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_WorldBuilder.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"
#include "LandscapeProxy.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace
{
	template<typename T>
	T* LoadGameAsset(const FString& InPath, FString& OutError)
	{
		FString ObjectPath = InPath;
		T* Asset = LoadObject<T>(nullptr, *ObjectPath);
		if (!Asset && InPath.StartsWith(TEXT("/Game/")) && !InPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(InPath);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *InPath, *AssetName);
			Asset = LoadObject<T>(nullptr, *ObjectPath);
		}
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Could not load asset '%s'"), *InPath);
		}
		return Asset;
	}

	TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("location"), UnrealClaudeJsonUtils::VectorToJson(Transform.GetLocation()));
		Result->SetObjectField(TEXT("rotation"), UnrealClaudeJsonUtils::RotatorToJson(Transform.Rotator()));
		Result->SetObjectField(TEXT("scale"), UnrealClaudeJsonUtils::VectorToJson(Transform.GetScale3D()));
		return Result;
	}

	AStaticMeshActor* SpawnStaticMeshActor(UWorld* World, UStaticMesh* Mesh, const FTransform& Transform, const FString& Label)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, SpawnParams);
		if (Actor)
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			if (!Label.IsEmpty())
			{
				Actor->SetActorLabel(Label);
			}
		}
		return Actor;
	}
}

FMCPToolInfo FMCPTool_WorldBuilder::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("world_builder");
	Info.Description = TEXT(
		"Build and inspect environments using the project's real assets. Operations: inspect_static_mesh, place_static_mesh, build_room_shell, scatter_foliage, inspect_landscapes. "
		"Use inspect_static_mesh before modular placement. build_room_shell creates a simple perimeter from one wall module with explicit dimensions; it does not guess the module's artistic orientation. "
		"scatter_foliage uses an existing Foliage Type, so its collision, shadow, culling, and materials stay intact. Landscape editing is inspection-only for now.");
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("inspect_static_mesh, place_static_mesh, build_room_shell, scatter_foliage, or inspect_landscapes"), true),
		FMCPToolParameter(TEXT("mesh_path"), TEXT("string"), TEXT("Static Mesh asset path, e.g. /Game/Environment/SM_Wall"), false),
		FMCPToolParameter(TEXT("foliage_type_path"), TEXT("string"), TEXT("Existing Foliage Type asset path"), false),
		FMCPToolParameter(TEXT("location"), TEXT("object"), TEXT("World location {x,y,z}"), false),
		FMCPToolParameter(TEXT("rotation"), TEXT("object"), TEXT("World rotation {pitch,yaw,roll}"), false),
		FMCPToolParameter(TEXT("scale"), TEXT("object"), TEXT("World scale {x,y,z}"), false),
		FMCPToolParameter(TEXT("label"), TEXT("string"), TEXT("Optional Actor Label"), false),
		FMCPToolParameter(TEXT("width"), TEXT("number"), TEXT("Room width in Unreal centimetres"), false),
		FMCPToolParameter(TEXT("depth"), TEXT("number"), TEXT("Room depth in Unreal centimetres"), false),
		FMCPToolParameter(TEXT("levels"), TEXT("number"), TEXT("Whole-number vertical wall levels for build_room_shell"), false),
		FMCPToolParameter(TEXT("area_size"), TEXT("object"), TEXT("Foliage scatter area in cm {x,y}; centred on location"), false),
		FMCPToolParameter(TEXT("count"), TEXT("number"), TEXT("Number of foliage instances; capped at 5000"), false),
		FMCPToolParameter(TEXT("seed"), TEXT("number"), TEXT("Optional deterministic foliage scatter seed"), false),
		FMCPToolParameter(TEXT("min_scale"), TEXT("number"), TEXT("Minimum uniform foliage scale"), false),
		FMCPToolParameter(TEXT("max_scale"), TEXT("number"), TEXT("Maximum uniform foliage scale"), false),
		FMCPToolParameter(TEXT("surface_z"), TEXT("number"), TEXT("Optional fallback placement height if no hit is found"), false)
	};
	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_WorldBuilder::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, ParamError)) return ParamError.GetValue();
	if (Operation.Equals(TEXT("inspect_static_mesh"), ESearchCase::IgnoreCase)) return ExecuteInspectStaticMesh(Params);
	if (Operation.Equals(TEXT("place_static_mesh"), ESearchCase::IgnoreCase)) return ExecutePlaceStaticMesh(Params);
	if (Operation.Equals(TEXT("build_room_shell"), ESearchCase::IgnoreCase)) return ExecuteBuildRoomShell(Params);
	if (Operation.Equals(TEXT("scatter_foliage"), ESearchCase::IgnoreCase)) return ExecuteScatterFoliage(Params);
	if (Operation.Equals(TEXT("inspect_landscapes"), ESearchCase::IgnoreCase)) return ExecuteInspectLandscapes(Params);
	return FMCPToolResult::Error(FString::Printf(TEXT("Unknown world_builder operation '%s'"), *Operation));
}

FMCPToolResult FMCPTool_WorldBuilder::ExecuteInspectStaticMesh(const TSharedRef<FJsonObject>& Params)
{
	FString MeshPath; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("mesh_path"), MeshPath, ParamError)) return ParamError.GetValue();
	FString Error; UStaticMesh* Mesh = LoadGameAsset<UStaticMesh>(MeshPath, Error);
	if (!Mesh) return FMCPToolResult::Error(Error);

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const FVector Dimensions = Bounds.BoxExtent * 2.0;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Data->SetObjectField(TEXT("dimensions_cm"), UnrealClaudeJsonUtils::VectorToJson(Dimensions));
	Data->SetObjectField(TEXT("origin_to_bounds_center_cm"), UnrealClaudeJsonUtils::VectorToJson(Bounds.Origin));
	Data->SetNumberField(TEXT("triangle_count"), Mesh->GetNumTriangles(0));
	Data->SetNumberField(TEXT("material_slots"), Mesh->GetStaticMaterials().Num());
	Data->SetBoolField(TEXT("nanite_enabled"), Mesh->IsNaniteEnabled());
	return FMCPToolResult::Success(TEXT("Inspected Static Mesh"), Data);
}

FMCPToolResult FMCPTool_WorldBuilder::ExecutePlaceStaticMesh(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr; if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString MeshPath; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("mesh_path"), MeshPath, ParamError)) return ParamError.GetValue();
	FString Error; UStaticMesh* Mesh = LoadGameAsset<UStaticMesh>(MeshPath, Error);
	if (!Mesh) return FMCPToolResult::Error(Error);
	const FTransform Transform(ExtractRotatorParam(Params, TEXT("rotation")), ExtractVectorParam(Params, TEXT("location")), ExtractScaleParam(Params, TEXT("scale")));

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealClaude", "PlaceStaticMesh", "MCP Place Static Mesh"));
	AStaticMeshActor* Actor = SpawnStaticMeshActor(World, Mesh, Transform, ExtractOptionalString(Params, TEXT("label")));
	if (!Actor) return FMCPToolResult::Error(TEXT("Failed to spawn StaticMeshActor"));
	MarkActorDirty(Actor);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());
	Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Data->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));
	return FMCPToolResult::Success(TEXT("Placed Static Mesh actor"), Data);
}

FMCPToolResult FMCPTool_WorldBuilder::ExecuteBuildRoomShell(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr; if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString MeshPath; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("mesh_path"), MeshPath, ParamError)) return ParamError.GetValue();
	FString Error; UStaticMesh* Mesh = LoadGameAsset<UStaticMesh>(MeshPath, Error);
	if (!Mesh) return FMCPToolResult::Error(Error);

	const double Width = ExtractOptionalNumber<double>(Params, TEXT("width"), 0.0);
	const double Depth = ExtractOptionalNumber<double>(Params, TEXT("depth"), 0.0);
	const int32 Levels = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("levels"), 1), 1, 20);
	if (Width <= 0.0 || Depth <= 0.0)
	{
		return FMCPToolResult::Error(TEXT("build_room_shell requires positive width and depth in centimetres."));
	}

	const FVector MeshDimensions = Mesh->GetBounds().BoxExtent * 2.0;
	if (MeshDimensions.X < 1.0 || MeshDimensions.Z < 1.0)
	{
		return FMCPToolResult::Error(TEXT("The wall mesh has invalid local X/Z bounds. Inspect it before using build_room_shell."));
	}

	const FVector Center = ExtractVectorParam(Params, TEXT("location"));
	const FString Prefix = ExtractOptionalString(Params, TEXT("label"), TEXT("MCP_Room"));
	const int32 AlongX = FMath::Max(1, FMath::RoundToInt(Width / MeshDimensions.X));
	const int32 AlongY = FMath::Max(1, FMath::RoundToInt(Depth / MeshDimensions.X));
	const double ActualWidth = AlongX * MeshDimensions.X;
	const double ActualDepth = AlongY * MeshDimensions.X;

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealClaude", "BuildRoomShell", "MCP Build Modular Room Shell"));
	TArray<TSharedPtr<FJsonValue>> Spawned;
	auto PlaceWall = [&](const FVector& Location, float Yaw, int32 Index)
	{
		AStaticMeshActor* Actor = SpawnStaticMeshActor(World, Mesh, FTransform(FRotator(0.0f, Yaw, 0.0f), Location), FString::Printf(TEXT("%s_%03d"), *Prefix, Index));
		if (Actor)
		{
			MarkActorDirty(Actor);
			TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetStringField(TEXT("actor_name"), Actor->GetName());
			Info->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Spawned.Add(MakeShared<FJsonValueObject>(Info));
		}
	};

	int32 Index = 1;
	for (int32 Level = 0; Level < Levels; ++Level)
	{
		const double Z = Center.Z + (MeshDimensions.Z * 0.5) + (Level * MeshDimensions.Z);
		for (int32 X = 0; X < AlongX; ++X)
		{
			const double OffsetX = -ActualWidth * 0.5 + MeshDimensions.X * 0.5 + X * MeshDimensions.X;
			PlaceWall(FVector(Center.X + OffsetX, Center.Y - ActualDepth * 0.5, Z), 0.0f, Index++);
			PlaceWall(FVector(Center.X + OffsetX, Center.Y + ActualDepth * 0.5, Z), 180.0f, Index++);
		}
		for (int32 Y = 0; Y < AlongY; ++Y)
		{
			const double OffsetY = -ActualDepth * 0.5 + MeshDimensions.X * 0.5 + Y * MeshDimensions.X;
			PlaceWall(FVector(Center.X - ActualWidth * 0.5, Center.Y + OffsetY, Z), -90.0f, Index++);
			PlaceWall(FVector(Center.X + ActualWidth * 0.5, Center.Y + OffsetY, Z), 90.0f, Index++);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("spawned"), Spawned);
	Data->SetNumberField(TEXT("actor_count"), Spawned.Num());
	Data->SetObjectField(TEXT("actual_outer_dimensions_cm"), UnrealClaudeJsonUtils::VectorToJson(FVector(ActualWidth, ActualDepth, Levels * MeshDimensions.Z)));
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Built modular room shell"), Data);
	Result.Warnings.Add(TEXT("This uses the mesh's local X axis as its length and assumes a centered pivot. Inspect and test one wall module first; doors, windows, corners, ceilings, and art direction remain explicit placement choices."));
	return Result;
}

FMCPToolResult FMCPTool_WorldBuilder::ExecuteScatterFoliage(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr; if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString FoliagePath; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("foliage_type_path"), FoliagePath, ParamError)) return ParamError.GetValue();
	FString Error; UFoliageType* FoliageType = LoadGameAsset<UFoliageType>(FoliagePath, Error);
	if (!FoliageType) return FMCPToolResult::Error(Error);

	const FVector Center = ExtractVectorParam(Params, TEXT("location"));
	const FVector Area = ExtractVectorParam(Params, TEXT("area_size"));
	if (Area.X <= 0.0 || Area.Y <= 0.0) return FMCPToolResult::Error(TEXT("scatter_foliage requires area_size with positive x and y values."));
	const int32 Count = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("count"), 0), 1, 5000);
	const int32 Seed = ExtractOptionalNumber<int32>(Params, TEXT("seed"), 1337);
	const float MinScale = FMath::Max(0.01f, ExtractOptionalNumber<float>(Params, TEXT("min_scale"), 0.85f));
	const float MaxScale = FMath::Max(MinScale, ExtractOptionalNumber<float>(Params, TEXT("max_scale"), 1.15f));
	const float FallbackZ = ExtractOptionalNumber<float>(Params, TEXT("surface_z"), Center.Z);

	FRandomStream Random(Seed);
	TArray<FTransform> Transforms;
	Transforms.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector Sample(Center.X + Random.FRandRange(-Area.X * 0.5f, Area.X * 0.5f), Center.Y + Random.FRandRange(-Area.Y * 0.5f, Area.Y * 0.5f), FallbackZ);
		FHitResult Hit;
		const FVector TraceStart(Sample.X, Sample.Y, FMath::Max(Center.Z + 100000.0f, FallbackZ + 100000.0f));
		const FVector TraceEnd(Sample.X, Sample.Y, FallbackZ - 100000.0f);
		const bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_WorldStatic);
		const FVector Location = bHit ? Hit.ImpactPoint : Sample;
		const float Scale = Random.FRandRange(MinScale, MaxScale);
		Transforms.Emplace(FRotator(0.0f, Random.FRandRange(0.0f, 360.0f), 0.0f), Location, FVector(Scale));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealClaude", "ScatterFoliage", "MCP Scatter Foliage"));
	AInstancedFoliageActor::AddInstances(World, FoliageType, Transforms);
	MarkWorldDirty(World);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("instances_added"), Transforms.Num());
	Data->SetNumberField(TEXT("seed"), Seed);
	Data->SetObjectField(TEXT("area_center"), UnrealClaudeJsonUtils::VectorToJson(Center));
	Data->SetObjectField(TEXT("area_size_cm"), UnrealClaudeJsonUtils::VectorToJson(Area));
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Scattered foliage instances"), Data);
	Result.Warnings.Add(TEXT("Instances inherit the existing Foliage Type settings. Review the scatter in-editor before saving the level; use a dedicated Foliage Type for each performance profile."));
	return Result;
}

FMCPToolResult FMCPTool_WorldBuilder::ExecuteInspectLandscapes(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr; if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	TArray<TSharedPtr<FJsonValue>> Landscapes;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Landscape = *It;
		if (!Landscape) continue;
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("actor_name"), Landscape->GetName());
		Info->SetStringField(TEXT("actor_label"), Landscape->GetActorLabel());
		Info->SetObjectField(TEXT("transform"), TransformToJson(Landscape->GetActorTransform()));
		Info->SetObjectField(TEXT("bounds_cm"), UnrealClaudeJsonUtils::VectorToJson(Landscape->GetComponentsBoundingBox().GetSize()));
		Info->SetStringField(TEXT("material"), Landscape->LandscapeMaterial ? Landscape->LandscapeMaterial->GetPathName() : TEXT("None"));
		Landscapes.Add(MakeShared<FJsonValueObject>(Info));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("landscapes"), Landscapes);
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Inspected landscapes"), Data);
	Result.Warnings.Add(TEXT("Landscape sculpting and layer painting are intentionally not automated yet. They need an explicit target landscape, edit layer, operation, brush bounds, strength, and falloff to be safe."));
	return Result;
}
