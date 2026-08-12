// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: practical Level Sequence editing.
 *
 * This intentionally covers the reliable editor automation layer: inspect a
 * sequence, bind an actor, key actor transforms, and place an existing
 * skeletal animation asset on a binding. Control Rig keying remains a
 * separate follow-up because its control names are project-specific.
 */
class FMCPTool_Sequencer : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteInspect(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteBindActor(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetPlaybackRange(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetTransformKey(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddAnimationClip(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddAudioClip(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteInspectControlRigs(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetControlRigTransformKey(const TSharedRef<FJsonObject>& Params);
};
