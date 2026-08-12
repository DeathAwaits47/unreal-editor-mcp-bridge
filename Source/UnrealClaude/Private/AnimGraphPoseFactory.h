// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UAnimBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Top-level AnimGraph pose-node authoring.
 *
 * Where FAnimAssetNodeFactory handles nodes *inside a state* (sequence/blendspace
 * players) and the state-machine layer is covered elsewhere, this factory covers
 * the main AnimGraph pose graph: Layered Blend Per Bone, Use/Save Cached Pose,
 * Blend Poses (BlendListByEnum/Bool), and connecting their pose pins.
 *
 * Nodes are addressed by their native NodeGuid string, so pre-existing hand-made
 * nodes (not created via MCP) are fully reachable for inspect/modify/connect.
 */
class FAnimGraphPoseFactory
{
public:
	/** List every node in the main AnimGraph with GUID, class, pins, and type-specific detail. */
	static TSharedPtr<FJsonObject> ListNodes(UAnimBlueprint* AnimBP, FString& OutError);

	/**
	 * Create a pose node in the main AnimGraph.
	 * NodeType (friendly): use_cached_pose, save_cached_pose, layered_bone_blend,
	 * blend_list_by_enum, blend_list_by_bool, two_bone_ik, modify_bone, sequence_player.
	 * Any other value is resolved as /Script/AnimGraph.AnimGraphNode_<NodeType>.
	 * Params may include: cache_name, pos_x, pos_y.
	 */
	static UEdGraphNode* CreateNode(
		UAnimBlueprint* AnimBP,
		const FString& NodeType,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutNodeId,
		FString& OutError);

	/**
	 * Set a property on a pose node (addressed by GUID).
	 * Recognized params: bone_name, blend_depth, layer_index, mesh_space_rotation_blend,
	 * cache_name; plus generic {property, value} reflected onto the node's Node struct.
	 */
	static bool SetNodeProperty(
		UAnimBlueprint* AnimBP,
		const FString& NodeId,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutError);

	/** Connect a pose output of one node to a named pose input pin of another. */
	static bool ConnectPose(
		UAnimBlueprint* AnimBP,
		const FString& FromNodeId,
		const FString& FromPin,
		const FString& ToNodeId,
		const FString& ToPin,
		FString& OutError);

private:
	static UClass* ResolveNodeClass(const FString& NodeType);
	static TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node);
};
