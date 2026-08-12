// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * Read-first performance and material-cost inspection for the active UE level.
 *
 * Scene audits report measurable editor data (instances, triangles, shadow
 * casters, lights, Nanite, and material use). Exact GPU/CPU frame time still
 * requires a PIE or packaged runtime profile.
 */
class FMCPTool_Performance : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteSceneAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteMaterialAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRuntimeProfileCommand(const TSharedRef<FJsonObject>& Params);
};
