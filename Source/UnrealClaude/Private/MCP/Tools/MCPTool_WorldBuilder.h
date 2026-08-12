// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP tool for the reliable, asset-aware portion of environment building.
 *
 * It deliberately uses existing Static Mesh and Foliage Type assets. That
 * means a project's collision, materials, culling, shadow, and foliage
 * settings remain authoritative instead of being silently replaced.
 */
class FMCPTool_WorldBuilder : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteInspectStaticMesh(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecutePlaceStaticMesh(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteBuildRoomShell(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteScatterFoliage(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteInspectLandscapes(const TSharedRef<FJsonObject>& Params);
};
