// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UWidgetBlueprint;
class UWidget;
class UWidgetTree;
class UPanelWidget;

/**
 * MCP Tool for editing UMG Widget Blueprints (the designer surface).
 *
 * Unlike blueprint_modify (which only touches a Widget Blueprint's event
 * graph logic), this tool works on the visual widget tree: creating widgets,
 * parenting them into panels, setting their properties and layout slots, and
 * compiling the result.
 *
 * Operations:
 * - inspect: dump the widget tree hierarchy (names, classes, slots)
 * - create_widget_blueprint: create a new UWidgetBlueprint asset with a root panel
 * - add_widget: construct a widget of a given class under a parent panel
 * - remove_widget: remove a widget from the tree
 * - set_widget_property: set a reflected property on a widget (text, color, brush...)
 * - set_slot_property: set layout on a widget's slot (position/size/anchors/padding...)
 * - compile: mark structurally modified + compile + save the Widget Blueprint
 */
class FMCPTool_Widget : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	FMCPToolResult ExecuteInspect(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteCreateWidgetBlueprint(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteAddWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetSlotProperty(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteCompile(const TSharedRef<FJsonObject>& Params);

	/** Load a Widget Blueprint by asset path (accepts /Game/... with or without .Name). */
	UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath, FString& OutError) const;

	/** Resolve a widget class from a short UMG name (e.g. "TextBlock") or full asset path. */
	UClass* ResolveWidgetClass(const FString& ClassNameOrPath) const;

	/** Recursively serialize a widget and its children into JSON. */
	TSharedPtr<FJsonObject> SerializeWidget(UWidget* Widget) const;

	/** Set one reflected property on an object from a JSON value. Returns false + reason on failure. */
	bool ApplyJsonProperty(UObject* Target, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError) const;

	/** Compile + mark modified + save a Widget Blueprint. */
	void FinalizeWidgetBlueprint(UWidgetBlueprint* WidgetBP) const;
};
