// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

class AActor;
class FProperty;

/**
 * Reads and edits placed dialogue / radio trigger instances without changing
 * their Blueprint class defaults. Property discovery is reflection-based so it
 * works with project-specific trigger variants.
 */
class FMCPTool_NarrativeTrigger : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteList(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRead(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSubtitleAudit(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteUpdateSubtitles(const TSharedRef<FJsonObject>& Params);

	bool IsNarrativeTrigger(const AActor* Actor, const FString& ClassFilter) const;
	TSharedPtr<FJsonObject> BuildTriggerJson(AActor* Actor) const;
	FProperty* FindPropertyByAliases(AActor* Actor, const TArray<FString>& Aliases) const;
	bool SetTextByAliases(AActor* Actor, const TArray<FString>& Aliases, const FString& Value, FString& OutProperty, FString& OutError) const;
	bool SetBoolByAliases(AActor* Actor, const TArray<FString>& Aliases, bool Value, FString& OutProperty, FString& OutError) const;
	bool SetNumberByAliases(AActor* Actor, const TArray<FString>& Aliases, double Value, FString& OutProperty, FString& OutError) const;
};
