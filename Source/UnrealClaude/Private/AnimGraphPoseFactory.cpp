// Copyright Natali Caggiano. All Rights Reserved.

#include "AnimGraphPoseFactory.h"
#include "AnimGraphEditor.h"
#include "AnimNodePinUtils.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_BlendListByEnum.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace
{
	UEdGraphNode* FindByGuidOrId(UEdGraph* Graph, const FString& NodeId)
	{
		// FAnimGraphEditor::FindNodeById already resolves both the MCP node-id
		// comment scheme and the native NodeGuid fallback.
		return FAnimGraphEditor::FindNodeById(Graph, NodeId);
	}

	UEdGraphPin* FindPoseOutput(UEdGraphNode* Node, FString& OutError)
	{
		FPinSearchConfig Config = FPinSearchConfig::Output({
			FName("Pose"), FName("Output"), FName("Output Pose"), FName("Result")
		}).WithCategory(UEdGraphSchema_K2::PC_Struct).WithNameContains(TEXT("Pose")).AcceptAny();
		return FAnimNodePinUtils::FindPinWithFallbacks(Node, Config, &OutError);
	}

	UEdGraphPin* FindPoseInput(UEdGraphNode* Node, const FString& PinName, FString& OutError)
	{
		if (!PinName.IsEmpty())
		{
			FPinSearchConfig Config = FPinSearchConfig::Input({ FName(*PinName) })
				.WithCategory(UEdGraphSchema_K2::PC_Struct);
			if (UEdGraphPin* Found = FAnimNodePinUtils::FindPinWithFallbacks(Node, Config, nullptr))
			{
				return Found;
			}
			// Direct exact match by string (covers dynamic pins like BlendPose_3).
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
				{
					return Pin;
				}
			}
			OutError = FString::Printf(TEXT("Input pose pin '%s' not found on target node."), *PinName);
			return nullptr;
		}

		FPinSearchConfig Config = FPinSearchConfig::Input({
			FName("Base Pose"), FName("Blend Pose"), FName("Pose"), FName("Input")
		}).WithCategory(UEdGraphSchema_K2::PC_Struct).AcceptAny();
		return FAnimNodePinUtils::FindPinWithFallbacks(Node, Config, &OutError);
	}
}

UClass* FAnimGraphPoseFactory::ResolveNodeClass(const FString& NodeType)
{
	const FString T = NodeType.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
	if (T == TEXT("usecachedpose")) return UAnimGraphNode_UseCachedPose::StaticClass();
	if (T == TEXT("savecachedpose")) return UAnimGraphNode_SaveCachedPose::StaticClass();
	if (T == TEXT("layeredboneblend") || T == TEXT("layeredblendperbone")) return UAnimGraphNode_LayeredBoneBlend::StaticClass();
	if (T == TEXT("blendlistbyenum") || T == TEXT("blendposesbyenum")) return UAnimGraphNode_BlendListByEnum::StaticClass();

	// Generic fallback: /Script/AnimGraph.AnimGraphNode_<Type>
	FString ClassName = NodeType;
	if (!ClassName.StartsWith(TEXT("AnimGraphNode_")))
	{
		ClassName = FString::Printf(TEXT("AnimGraphNode_%s"), *NodeType);
	}
	if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AnimGraph.%s"), *ClassName)))
	{
		return Found;
	}
	return LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AnimGraph.%s"), *ClassName));
}

TSharedPtr<FJsonObject> FAnimGraphPoseFactory::SerializeNode(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Node) return Json;

	Json->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

	// Pins
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Pin->PinName.ToString());
		P->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		P->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		P->SetNumberField(TEXT("links"), Pin->LinkedTo.Num());
		Pins.Add(MakeShared<FJsonValueObject>(P));
	}
	Json->SetArrayField(TEXT("pins"), Pins);

	// Type-specific detail relevant to layered-blend / cached-pose work.
	if (UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(Node))
	{
		Json->SetStringField(TEXT("cache_name"), Save->CacheName);
	}
	else if (UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		Json->SetStringField(TEXT("references_cache"),
			Use->SaveCachedPoseNode.IsValid() ? Use->SaveCachedPoseNode->CacheName : TEXT("(unresolved)"));
	}
	else if (UAnimGraphNode_LayeredBoneBlend* Layered = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
	{
		Json->SetBoolField(TEXT("mesh_space_rotation_blend"), Layered->Node.bMeshSpaceRotationBlend);
		TArray<TSharedPtr<FJsonValue>> Layers;
		for (int32 i = 0; i < Layered->Node.LayerSetup.Num(); ++i)
		{
			TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
			L->SetNumberField(TEXT("layer_index"), i);
			TArray<TSharedPtr<FJsonValue>> Filters;
			for (const FBranchFilter& BF : Layered->Node.LayerSetup[i].BranchFilters)
			{
				TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("bone_name"), BF.BoneName.ToString());
				F->SetNumberField(TEXT("blend_depth"), BF.BlendDepth);
				Filters.Add(MakeShared<FJsonValueObject>(F));
			}
			L->SetArrayField(TEXT("branch_filters"), Filters);
			Layers.Add(MakeShared<FJsonValueObject>(L));
		}
		Json->SetArrayField(TEXT("layer_setup"), Layers);
	}

	return Json;
}

TSharedPtr<FJsonObject> FAnimGraphPoseFactory::ListNodes(UAnimBlueprint* AnimBP, FString& OutError)
{
	UEdGraph* AnimGraph = FAnimGraphEditor::FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph) return nullptr;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Node)));
		}
	}
	Data->SetArrayField(TEXT("nodes"), Nodes);
	Data->SetNumberField(TEXT("count"), Nodes.Num());
	return Data;
}

UEdGraphNode* FAnimGraphPoseFactory::CreateNode(
	UAnimBlueprint* AnimBP,
	const FString& NodeType,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutNodeId,
	FString& OutError)
{
	UEdGraph* AnimGraph = FAnimGraphEditor::FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph) return nullptr;

	UClass* NodeClass = ResolveNodeClass(NodeType);
	if (!NodeClass || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Unknown or invalid anim node type '%s'."), *NodeType);
		return nullptr;
	}

	AnimGraph->Modify();
	UAnimGraphNode_Base* NewNode = NewObject<UAnimGraphNode_Base>(AnimGraph, NodeClass, NAME_None, RF_Transactional);
	if (!NewNode)
	{
		OutError = TEXT("Failed to allocate anim node.");
		return nullptr;
	}
	AnimGraph->AddNode(NewNode, false, false);
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	int32 PosX = 0, PosY = 0;
	if (Params.IsValid())
	{
		double D;
		if (Params->TryGetNumberField(TEXT("pos_x"), D)) PosX = static_cast<int32>(D);
		if (Params->TryGetNumberField(TEXT("pos_y"), D)) PosY = static_cast<int32>(D);
	}
	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;

	FString CacheName;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("cache_name"), CacheName);

	if (UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(NewNode))
	{
		Save->CacheName = CacheName.IsEmpty() ? TEXT("NewSavedPose") : CacheName;
	}
	else if (UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(NewNode))
	{
		if (!CacheName.IsEmpty())
		{
			for (UEdGraphNode* Node : AnimGraph->Nodes)
			{
				if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
				{
					if (SaveNode->CacheName.Equals(CacheName, ESearchCase::IgnoreCase))
					{
						Use->SaveCachedPoseNode = SaveNode;
						break;
					}
				}
			}
			if (!Use->SaveCachedPoseNode.IsValid())
			{
				OutError = FString::Printf(TEXT("No 'Save cached pose' named '%s' found to reference (node still created)."), *CacheName);
			}
			Use->ReconstructNode();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	OutNodeId = NewNode->NodeGuid.ToString();
	return NewNode;
}

bool FAnimGraphPoseFactory::SetNodeProperty(
	UAnimBlueprint* AnimBP,
	const FString& NodeId,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutError)
{
	UEdGraph* AnimGraph = FAnimGraphEditor::FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph) return false;

	UEdGraphNode* Node = FindByGuidOrId(AnimGraph, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node '%s' not found in AnimGraph."), *NodeId);
		return false;
	}

	Node->Modify();
	bool bAnyApplied = false;

	FString StrVal;
	double NumVal;
	bool BoolVal;

	if (UAnimGraphNode_LayeredBoneBlend* Layered = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
	{
		const int32 LayerIndex = Params->HasField(TEXT("layer_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("layer_index"))) : 0;

		if (Params->TryGetStringField(TEXT("bone_name"), StrVal))
		{
			// Keep BlendPoses/LayerSetup in sync so pins reconstruct correctly.
			while (Layered->Node.LayerSetup.Num() <= LayerIndex) { Layered->Node.LayerSetup.AddDefaulted(); }
			while (Layered->Node.BlendPoses.Num() < Layered->Node.LayerSetup.Num()) { Layered->Node.BlendPoses.AddDefaulted(); }
			if (Layered->Node.LayerSetup[LayerIndex].BranchFilters.Num() == 0) { Layered->Node.LayerSetup[LayerIndex].BranchFilters.AddDefaulted(); }
			FBranchFilter& Filter = Layered->Node.LayerSetup[LayerIndex].BranchFilters[0];
			Filter.BoneName = FName(*StrVal);
			if (Params->TryGetNumberField(TEXT("blend_depth"), NumVal)) { Filter.BlendDepth = static_cast<int32>(NumVal); }
			bAnyApplied = true;
		}
		if (Params->TryGetBoolField(TEXT("mesh_space_rotation_blend"), BoolVal))
		{
			Layered->Node.bMeshSpaceRotationBlend = BoolVal;
			bAnyApplied = true;
		}
		if (bAnyApplied)
		{
			Layered->ReconstructNode();
		}
	}
	else if (UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(Node))
	{
		if (Params->TryGetStringField(TEXT("cache_name"), StrVal))
		{
			Save->CacheName = StrVal;
			bAnyApplied = true;
		}
	}
	else if (UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		if (Params->TryGetStringField(TEXT("cache_name"), StrVal))
		{
			for (UEdGraphNode* N : AnimGraph->Nodes)
			{
				if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(N))
				{
					if (SaveNode->CacheName.Equals(StrVal, ESearchCase::IgnoreCase))
					{
						Use->SaveCachedPoseNode = SaveNode;
						bAnyApplied = true;
						break;
					}
				}
			}
			if (!bAnyApplied)
			{
				OutError = FString::Printf(TEXT("No 'Save cached pose' named '%s' found."), *StrVal);
				return false;
			}
			Use->ReconstructNode();
		}
	}

	// Generic reflection fallback onto the node's inner Node struct (e.g. flags).
	if (!bAnyApplied)
	{
		FString PropName, PropValue;
		if (Params->TryGetStringField(TEXT("property"), PropName) && Params->TryGetStringField(TEXT("value"), PropValue))
		{
			FStructProperty* NodeStruct = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("Node"));
			if (NodeStruct)
			{
				void* StructPtr = NodeStruct->ContainerPtrToValuePtr<void>(Node);
				FProperty* Inner = FindFProperty<FProperty>(NodeStruct->Struct, FName(*PropName));
				if (Inner)
				{
					void* ValuePtr = Inner->ContainerPtrToValuePtr<void>(StructPtr);
					if (Inner->ImportText_Direct(*PropValue, ValuePtr, Node, PPF_None))
					{
						bAnyApplied = true;
					}
				}
			}
			if (!bAnyApplied)
			{
				OutError = FString::Printf(TEXT("Could not set property '%s' on %s."), *PropName, *Node->GetClass()->GetName());
				return false;
			}
		}
	}

	if (!bAnyApplied)
	{
		OutError = TEXT("No recognized property was provided (bone_name, blend_depth, mesh_space_rotation_blend, cache_name, or property+value).");
		return false;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	return true;
}

bool FAnimGraphPoseFactory::ConnectPose(
	UAnimBlueprint* AnimBP,
	const FString& FromNodeId,
	const FString& FromPin,
	const FString& ToNodeId,
	const FString& ToPin,
	FString& OutError)
{
	UEdGraph* AnimGraph = FAnimGraphEditor::FindAnimGraph(AnimBP, OutError);
	if (!AnimGraph) return false;

	UEdGraphNode* From = FindByGuidOrId(AnimGraph, FromNodeId);
	UEdGraphNode* To = FindByGuidOrId(AnimGraph, ToNodeId);
	if (!From) { OutError = FString::Printf(TEXT("from_node '%s' not found."), *FromNodeId); return false; }
	if (!To) { OutError = FString::Printf(TEXT("to_node '%s' not found."), *ToNodeId); return false; }

	UEdGraphPin* OutPin = nullptr;
	if (!FromPin.IsEmpty())
	{
		for (UEdGraphPin* Pin : From->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().Equals(FromPin, ESearchCase::IgnoreCase))
			{
				OutPin = Pin; break;
			}
		}
	}
	if (!OutPin)
	{
		OutPin = FindPoseOutput(From, OutError);
	}
	if (!OutPin) { return false; }

	UEdGraphPin* InPin = FindPoseInput(To, ToPin, OutError);
	if (!InPin) { return false; }

	// Pose inputs take a single link; clear any existing one first.
	InPin->BreakAllPinLinks();
	OutPin->MakeLinkTo(InPin);

	AnimGraph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	return true;
}
