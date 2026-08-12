// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Widget.h"
#include "UnrealClaudeModule.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

FMCPToolInfo FMCPTool_Widget::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("widget");
	Info.Description = TEXT(
		"Edit UMG Widget Blueprints on the designer surface (not just their event graph). "
		"Operations: inspect, create_widget_blueprint, add_widget, remove_widget, set_widget_property, set_slot_property, compile. "
		"Widgets are addressed by their name within the widget tree; parents must be panel widgets (CanvasPanel, VerticalBox, Overlay, etc.).");

	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("inspect, create_widget_blueprint, add_widget, remove_widget, set_widget_property, set_slot_property, or compile"), true),
		FMCPToolParameter(TEXT("widget_blueprint"), TEXT("string"), TEXT("Widget Blueprint asset path, e.g. /Game/UI/WBP_HUD (required for all ops except create_widget_blueprint uses package_path+asset_name)"), false),
		FMCPToolParameter(TEXT("asset_name"), TEXT("string"), TEXT("New Widget Blueprint name (create_widget_blueprint)"), false),
		FMCPToolParameter(TEXT("package_path"), TEXT("string"), TEXT("Package path for a new Widget Blueprint; default /Game/UI/"), false),
		FMCPToolParameter(TEXT("widget_class"), TEXT("string"), TEXT("Widget class for add_widget: short UMG name (TextBlock, Button, Image, VerticalBox, CanvasPanel...) or a /Game/... UserWidget path"), false),
		FMCPToolParameter(TEXT("widget_name"), TEXT("string"), TEXT("Name for the widget being added, or the target widget for property/remove ops"), false),
		FMCPToolParameter(TEXT("parent_name"), TEXT("string"), TEXT("Name of the parent panel widget to add into; defaults to the root panel"), false),
		FMCPToolParameter(TEXT("root_class"), TEXT("string"), TEXT("Root panel class for a new Widget Blueprint; default CanvasPanel"), false),
		FMCPToolParameter(TEXT("properties"), TEXT("object"), TEXT("For set_widget_property: {PropertyName: value}. Text properties accept a plain string; colors accept {r,g,b,a}."), false),
		FMCPToolParameter(TEXT("slot"), TEXT("object"), TEXT("For set_slot_property: CanvasPanelSlot fields {position:{x,y}, size:{x,y}, anchors:{minx,miny,maxx,maxy}, alignment:{x,y}, z_order, auto_size}; other slot types use reflected fields (padding, h_align, v_align)."), false)
	};

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_Widget::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error)) return Error.GetValue();

	if (Operation.Equals(TEXT("inspect"), ESearchCase::IgnoreCase)) return ExecuteInspect(Params);
	if (Operation.Equals(TEXT("create_widget_blueprint"), ESearchCase::IgnoreCase)) return ExecuteCreateWidgetBlueprint(Params);
	if (Operation.Equals(TEXT("add_widget"), ESearchCase::IgnoreCase)) return ExecuteAddWidget(Params);
	if (Operation.Equals(TEXT("remove_widget"), ESearchCase::IgnoreCase)) return ExecuteRemoveWidget(Params);
	if (Operation.Equals(TEXT("set_widget_property"), ESearchCase::IgnoreCase)) return ExecuteSetWidgetProperty(Params);
	if (Operation.Equals(TEXT("set_slot_property"), ESearchCase::IgnoreCase)) return ExecuteSetSlotProperty(Params);
	if (Operation.Equals(TEXT("compile"), ESearchCase::IgnoreCase)) return ExecuteCompile(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown widget operation '%s'. Valid: inspect, create_widget_blueprint, add_widget, remove_widget, set_widget_property, set_slot_property, compile"),
		*Operation));
}

UWidgetBlueprint* FMCPTool_Widget::LoadWidgetBlueprint(const FString& AssetPath, FString& OutError) const
{
	FString ObjectPath = AssetPath;
	UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!WidgetBP && AssetPath.StartsWith(TEXT("/Game/")) && !AssetPath.Contains(TEXT(".")))
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
		WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	}
	if (!WidgetBP)
	{
		OutError = FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath);
	}
	return WidgetBP;
}

UClass* FMCPTool_Widget::ResolveWidgetClass(const FString& ClassNameOrPath) const
{
	// Custom UserWidget by asset path.
	if (ClassNameOrPath.StartsWith(TEXT("/Game/")))
	{
		FString ClassPath = ClassNameOrPath;
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			ClassPath += TEXT("_C");
		}
		if (UClass* Loaded = LoadClass<UWidget>(nullptr, *ClassPath))
		{
			return Loaded;
		}
	}

	// Engine UMG classes live under /Script/UMG.
	if (UClass* EngineClass = LoadClass<UWidget>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *ClassNameOrPath)))
	{
		return EngineClass;
	}

	// Last resort: a fully-qualified script path passed verbatim.
	return LoadClass<UWidget>(nullptr, *ClassNameOrPath);
}

TSharedPtr<FJsonObject> FMCPTool_Widget::SerializeWidget(UWidget* Widget) const
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	if (!Widget)
	{
		return Node;
	}
	Node->SetStringField(TEXT("name"), Widget->GetName());
	Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	if (Widget->Slot)
	{
		Node->SetStringField(TEXT("slot_type"), Widget->Slot->GetClass()->GetName());
	}

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			if (UWidget* Child = Panel->GetChildAt(Index))
			{
				Children.Add(MakeShared<FJsonValueObject>(SerializeWidget(Child)));
			}
		}
		Node->SetArrayField(TEXT("children"), Children);
	}
	return Node;
}

FMCPToolResult FMCPTool_Widget::ExecuteInspect(const TSharedRef<FJsonObject>& Params)
{
	FString Path;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("Widget Blueprint has no widget tree."));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("widget_blueprint"), WidgetBP->GetPathName());
	Data->SetStringField(TEXT("parent_class"), WidgetBP->ParentClass ? WidgetBP->ParentClass->GetName() : TEXT("None"));
	if (Tree->RootWidget)
	{
		Data->SetObjectField(TEXT("root"), SerializeWidget(Tree->RootWidget));
	}
	else
	{
		Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
	}

	// Flat name list for convenience.
	TArray<UWidget*> All;
	Tree->GetAllWidgets(All);
	TArray<TSharedPtr<FJsonValue>> Names;
	for (UWidget* W : All)
	{
		if (W) Names.Add(MakeShared<FJsonValueString>(W->GetName()));
	}
	Data->SetArrayField(TEXT("all_widget_names"), Names);

	return FMCPToolResult::Success(FString::Printf(TEXT("Inspected widget tree of %s (%d widgets)"), *WidgetBP->GetName(), All.Num()), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteCreateWidgetBlueprint(const TSharedRef<FJsonObject>& Params)
{
	FString AssetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_name"), AssetName, Error)) return Error.GetValue();

	FString PackagePath = ExtractOptionalString(Params, TEXT("package_path"), TEXT("/Game/UI/"));
	if (!PackagePath.EndsWith(TEXT("/"))) PackagePath += TEXT("/");

	const FString FullPackagePath = PackagePath + AssetName;
	if (FindPackage(nullptr, *FullPackagePath) || LoadObject<UWidgetBlueprint>(nullptr, *FullPackagePath))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at %s"), *FullPackagePath));
	}

	UPackage* Package = CreatePackage(*FullPackagePath);
	if (!Package) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *FullPackagePath));

	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = UUserWidget::StaticClass();

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Factory->FactoryCreateNew(
		UWidgetBlueprint::StaticClass(), Package, FName(*AssetName),
		RF_Public | RF_Standalone, nullptr, GWarn));

	if (!WidgetBP) return FMCPToolResult::Error(TEXT("Failed to create Widget Blueprint."));

	// Ensure a root panel exists so add_widget has something to parent into.
	if (WidgetBP->WidgetTree && !WidgetBP->WidgetTree->RootWidget)
	{
		const FString RootClassName = ExtractOptionalString(Params, TEXT("root_class"), TEXT("CanvasPanel"));
		UClass* RootClass = ResolveWidgetClass(RootClassName);
		if (!RootClass || !RootClass->IsChildOf(UPanelWidget::StaticClass()))
		{
			RootClass = UCanvasPanel::StaticClass();
		}
		UWidget* Root = WidgetBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootClass->GetName()));
		WidgetBP->WidgetTree->RootWidget = Root;
	}

	FAssetRegistryModule::AssetCreated(WidgetBP);
	FinalizeWidgetBlueprint(WidgetBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("widget_blueprint"), WidgetBP->GetPathName());
	Data->SetStringField(TEXT("root"), WidgetBP->WidgetTree && WidgetBP->WidgetTree->RootWidget ? WidgetBP->WidgetTree->RootWidget->GetName() : TEXT("None"));
	return FMCPToolResult::Success(FString::Printf(TEXT("Created Widget Blueprint %s"), *FullPackagePath), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteAddWidget(const TSharedRef<FJsonObject>& Params)
{
	FString Path, ClassName, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_class"), ClassName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);
	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("Widget Blueprint has no widget tree."));

	if (Tree->FindWidget(FName(*WidgetName)))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("A widget named '%s' already exists in this tree."), *WidgetName));
	}

	UClass* WidgetClass = ResolveWidgetClass(ClassName);
	if (!WidgetClass) return FMCPToolResult::Error(FString::Printf(TEXT("Could not resolve widget class '%s'."), *ClassName));

	// Resolve parent panel (defaults to root).
	UPanelWidget* Parent = nullptr;
	const FString ParentName = ExtractOptionalString(Params, TEXT("parent_name"));
	if (!ParentName.IsEmpty())
	{
		Parent = Cast<UPanelWidget>(Tree->FindWidget(FName(*ParentName)));
		if (!Parent) return FMCPToolResult::Error(FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName));
	}
	else
	{
		Parent = Cast<UPanelWidget>(Tree->RootWidget);
		if (!Parent) return FMCPToolResult::Error(TEXT("No parent_name given and the root widget is not a panel. Specify parent_name."));
	}

	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!NewWidget) return FMCPToolResult::Error(TEXT("Failed to construct widget."));

	UPanelSlot* Slot = Parent->AddChild(NewWidget);
	if (!Slot) return FMCPToolResult::Error(FString::Printf(TEXT("Parent '%s' rejected the child (panel may be full or incompatible)."), *Parent->GetName()));

	FinalizeWidgetBlueprint(WidgetBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	Data->SetStringField(TEXT("widget_class"), WidgetClass->GetName());
	Data->SetStringField(TEXT("parent"), Parent->GetName());
	Data->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
	return FMCPToolResult::Success(FString::Printf(TEXT("Added %s '%s' under '%s'"), *WidgetClass->GetName(), *NewWidget->GetName(), *Parent->GetName()), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);
	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("Widget Blueprint has no widget tree."));

	UWidget* Target = Tree->FindWidget(FName(*WidgetName));
	if (!Target) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found."), *WidgetName));

	if (Target == Tree->RootWidget)
	{
		return FMCPToolResult::Error(TEXT("Refusing to remove the root widget. Remove children instead or delete the asset."));
	}

	const bool bRemoved = Tree->RemoveWidget(Target);
	if (!bRemoved) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to remove widget '%s'."), *WidgetName));

	FinalizeWidgetBlueprint(WidgetBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("removed"), WidgetName);
	return FMCPToolResult::Success(FString::Printf(TEXT("Removed widget '%s'"), *WidgetName), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	const TSharedPtr<FJsonObject>* PropsObj;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj->IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: properties (object of {PropertyName: value})"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);
	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("Widget Blueprint has no widget tree."));

	UWidget* Target = Tree->FindWidget(FName(*WidgetName));
	if (!Target) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found."), *WidgetName));

	TArray<FString> Applied;
	TArray<FString> Failures;
	for (const auto& Pair : (*PropsObj)->Values)
	{
		FString FieldError;
		if (ApplyJsonProperty(Target, Pair.Key, Pair.Value, FieldError))
		{
			Applied.Add(Pair.Key);
		}
		else
		{
			Failures.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *FieldError));
		}
	}

	FinalizeWidgetBlueprint(WidgetBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("applied"), StringArrayToJsonArray(Applied));
	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Set %d propert%s on '%s'"), Applied.Num(), Applied.Num() == 1 ? TEXT("y") : TEXT("ies"), *WidgetName), Data);
	for (const FString& F : Failures) Result.Warnings.Add(F);
	return Result;
}

FMCPToolResult FMCPTool_Widget::ExecuteSetSlotProperty(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	const TSharedPtr<FJsonObject>* SlotObj;
	if (!Params->TryGetObjectField(TEXT("slot"), SlotObj) || !SlotObj->IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: slot (layout object)"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);
	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree) return FMCPToolResult::Error(TEXT("Widget Blueprint has no widget tree."));

	UWidget* Target = Tree->FindWidget(FName(*WidgetName));
	if (!Target) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found."), *WidgetName));
	if (!Target->Slot) return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' has no slot (is it parented?)."), *WidgetName));

	TArray<FString> Applied;
	TArray<FString> Failures;

	// CanvasPanelSlot gets first-class handling for the common layout fields.
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Target->Slot))
	{
		const TSharedPtr<FJsonObject>* Vec;
		double D = 0.0;
		if ((*SlotObj)->TryGetObjectField(TEXT("position"), Vec))
		{
			FVector2D P = CanvasSlot->GetPosition();
			(*Vec)->TryGetNumberField(TEXT("x"), P.X);
			(*Vec)->TryGetNumberField(TEXT("y"), P.Y);
			CanvasSlot->SetPosition(P);
			Applied.Add(TEXT("position"));
		}
		if ((*SlotObj)->TryGetObjectField(TEXT("size"), Vec))
		{
			FVector2D S = CanvasSlot->GetSize();
			(*Vec)->TryGetNumberField(TEXT("x"), S.X);
			(*Vec)->TryGetNumberField(TEXT("y"), S.Y);
			CanvasSlot->SetSize(S);
			Applied.Add(TEXT("size"));
		}
		if ((*SlotObj)->TryGetObjectField(TEXT("alignment"), Vec))
		{
			FVector2D A = CanvasSlot->GetAlignment();
			(*Vec)->TryGetNumberField(TEXT("x"), A.X);
			(*Vec)->TryGetNumberField(TEXT("y"), A.Y);
			CanvasSlot->SetAlignment(A);
			Applied.Add(TEXT("alignment"));
		}
		if ((*SlotObj)->TryGetObjectField(TEXT("anchors"), Vec))
		{
			FAnchors An = CanvasSlot->GetAnchors();
			(*Vec)->TryGetNumberField(TEXT("minx"), An.Minimum.X);
			(*Vec)->TryGetNumberField(TEXT("miny"), An.Minimum.Y);
			An.Maximum = An.Minimum;
			(*Vec)->TryGetNumberField(TEXT("maxx"), An.Maximum.X);
			(*Vec)->TryGetNumberField(TEXT("maxy"), An.Maximum.Y);
			CanvasSlot->SetAnchors(An);
			Applied.Add(TEXT("anchors"));
		}
		if ((*SlotObj)->TryGetNumberField(TEXT("z_order"), D))
		{
			CanvasSlot->SetZOrder(static_cast<int32>(D));
			Applied.Add(TEXT("z_order"));
		}
		bool bAuto = false;
		if ((*SlotObj)->TryGetBoolField(TEXT("auto_size"), bAuto))
		{
			CanvasSlot->SetAutoSize(bAuto);
			Applied.Add(TEXT("auto_size"));
		}
	}
	else
	{
		// Generic reflection for other slot types (VerticalBoxSlot padding/h_align/v_align, etc.).
		for (const auto& Pair : (*SlotObj)->Values)
		{
			FString FieldError;
			if (ApplyJsonProperty(Target->Slot, Pair.Key, Pair.Value, FieldError))
			{
				Applied.Add(Pair.Key);
			}
			else
			{
				Failures.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *FieldError));
			}
		}
	}

	Target->Slot->SynchronizeProperties();
	FinalizeWidgetBlueprint(WidgetBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("slot_type"), Target->Slot->GetClass()->GetName());
	Data->SetArrayField(TEXT("applied"), StringArrayToJsonArray(Applied));
	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Set %d slot field(s) on '%s'"), Applied.Num(), *WidgetName), Data);
	for (const FString& F : Failures) Result.Warnings.Add(F);
	return Result;
}

FMCPToolResult FMCPTool_Widget::ExecuteCompile(const TSharedRef<FJsonObject>& Params)
{
	FString Path;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("widget_blueprint"), Path, Error)) return Error.GetValue();

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, LoadError);
	if (!WidgetBP) return FMCPToolResult::Error(LoadError);

	FinalizeWidgetBlueprint(WidgetBP);

	const bool bHasErrors = (WidgetBP->Status == BS_Error);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("widget_blueprint"), WidgetBP->GetPathName());
	Data->SetBoolField(TEXT("compiled"), !bHasErrors);
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Compiled %s (%s)"), *WidgetBP->GetName(), bHasErrors ? TEXT("with errors") : TEXT("clean")), Data);
}

bool FMCPTool_Widget::ApplyJsonProperty(UObject* Target, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError) const
{
	if (!Target)
	{
		OutError = TEXT("null target");
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), FName(*PropertyName));
	if (!Property)
	{
		OutError = TEXT("no such property");
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);

	// FText: accept a plain string.
	if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		FString Str;
		if (!Value->TryGetString(Str))
		{
			OutError = TEXT("expected string for text property");
			return false;
		}
		TextProp->SetPropertyValue(ValuePtr, FText::FromString(Str));
		Target->Modify();
		return true;
	}

	// Struct with {r,g,b,a} → treat as FLinearColor / FSlateColor.
	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (Value->TryGetObject(Obj) && Obj->IsValid())
		{
			const bool bLooksLikeColor = (*Obj)->HasField(TEXT("r")) || (*Obj)->HasField(TEXT("g")) || (*Obj)->HasField(TEXT("b"));
			if (bLooksLikeColor)
			{
				FLinearColor Color(0, 0, 0, 1);
				(*Obj)->TryGetNumberField(TEXT("r"), Color.R);
				(*Obj)->TryGetNumberField(TEXT("g"), Color.G);
				(*Obj)->TryGetNumberField(TEXT("b"), Color.B);
				(*Obj)->TryGetNumberField(TEXT("a"), Color.A);

				if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
				{
					*reinterpret_cast<FLinearColor*>(ValuePtr) = Color;
					Target->Modify();
					return true;
				}
				if (StructProp->Struct == FSlateColor::StaticStruct())
				{
					*reinterpret_cast<FSlateColor*>(ValuePtr) = FSlateColor(Color);
					Target->Modify();
					return true;
				}
			}
		}
		// Fall through to ImportText for other struct shapes.
	}

	// Generic path: stringify the JSON value and use ImportText.
	FString StringValue;
	if (!Value->TryGetString(StringValue))
	{
		double Num;
		bool Bool;
		if (Value->TryGetNumber(Num))
		{
			StringValue = FString::SanitizeFloat(Num);
		}
		else if (Value->TryGetBool(Bool))
		{
			StringValue = Bool ? TEXT("true") : TEXT("false");
		}
		else
		{
			OutError = TEXT("unsupported value type for this property");
			return false;
		}
	}

	const TCHAR* Result = Property->ImportText_Direct(*StringValue, ValuePtr, Target, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("could not parse '%s'"), *StringValue);
		return false;
	}
	Target->Modify();
	return true;
}

void FMCPTool_Widget::FinalizeWidgetBlueprint(UWidgetBlueprint* WidgetBP) const
{
	if (!WidgetBP) return;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	UEditorAssetLibrary::SaveLoadedAsset(WidgetBP, false);
}
