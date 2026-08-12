// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_NarrativeTrigger.h"

#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundBase.h"
#include "UObject/UnrealType.h"

namespace
{
	bool HasAnyToken(const FString& Value, const TArray<FString>& Tokens)
	{
		for (const FString& Token : Tokens)
		{
			if (Value.Contains(Token, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	}

	FString GetObjectAssetPath(const FObjectPropertyBase* Property, const UObject* Container)
	{
		if (!Property || !Container) return FString();
		if (UObject* Value = Property->GetObjectPropertyValue_InContainer(Container)) return Value->GetPathName();
		return FString();
	}
}

FMCPToolInfo FMCPTool_NarrativeTrigger::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("narrative_trigger");
	Info.Description = TEXT(
		"Read, audit, and update placed voice, radio, or narrative trigger instances in the current level. "
		"It discovers audio, subtitle, speaker, boolean, and duration properties through reflection, then writes only the selected placed actor—not the master Blueprint or any other copied trigger. "
		"Use subtitle_audit before content work to find triggers missing a sound, English/Romanian text, a duration, or enabled subtitles.");
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("list, read, subtitle_audit, or update_subtitles"), true),
		FMCPToolParameter(TEXT("actor_name"), TEXT("string"), TEXT("Placed trigger actor name or label; required for read/update_subtitles"), false),
		FMCPToolParameter(TEXT("class_filter"), TEXT("string"), TEXT("Optional class-name filter; default matches VoiceTrigger, RadioVoiceTrigger, and NarrativeTrigger"), false),
		FMCPToolParameter(TEXT("name_filter"), TEXT("string"), TEXT("Optional actor-name or label substring filter"), false),
		FMCPToolParameter(TEXT("limit"), TEXT("number"), TEXT("Maximum trigger rows from list/audit; default 100, max 1000"), false),
		FMCPToolParameter(TEXT("english"), TEXT("string"), TEXT("English subtitle text to write"), false),
		FMCPToolParameter(TEXT("romanian"), TEXT("string"), TEXT("Romanian subtitle text to write"), false),
		FMCPToolParameter(TEXT("speaker"), TEXT("string"), TEXT("Optional speaker label to write"), false),
		FMCPToolParameter(TEXT("show_subtitle"), TEXT("boolean"), TEXT("Optional per-trigger subtitle enable flag to write"), false),
		FMCPToolParameter(TEXT("show_speaker"), TEXT("boolean"), TEXT("Optional per-trigger speaker-label visibility flag to write"), false),
		FMCPToolParameter(TEXT("duration"), TEXT("number"), TEXT("Optional per-trigger subtitle duration in seconds to write"), false)
	};
	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_NarrativeTrigger::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, ParamError)) return ParamError.GetValue();
	if (Operation.Equals(TEXT("list"), ESearchCase::IgnoreCase)) return ExecuteList(Params);
	if (Operation.Equals(TEXT("read"), ESearchCase::IgnoreCase)) return ExecuteRead(Params);
	if (Operation.Equals(TEXT("subtitle_audit"), ESearchCase::IgnoreCase)) return ExecuteSubtitleAudit(Params);
	if (Operation.Equals(TEXT("update_subtitles"), ESearchCase::IgnoreCase)) return ExecuteUpdateSubtitles(Params);
	return FMCPToolResult::Error(TEXT("Unknown narrative_trigger operation. Valid: list, read, subtitle_audit, update_subtitles."));
}

bool FMCPTool_NarrativeTrigger::IsNarrativeTrigger(const AActor* Actor, const FString& ClassFilter) const
{
	if (!Actor) return false;
	const FString ClassName = Actor->GetClass()->GetName();
	if (!ClassFilter.IsEmpty()) return ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase);
	return HasAnyToken(ClassName, { TEXT("VoiceTrigger"), TEXT("RadioVoiceTrigger"), TEXT("NarrativeTrigger") });
}

TSharedPtr<FJsonObject> FMCPTool_NarrativeTrigger::BuildTriggerJson(AActor* Actor) const
{
	TSharedPtr<FJsonObject> Data = BuildActorInfoWithTransformJson(Actor);
	TArray<TSharedPtr<FJsonValue>> AudioFields;
	TArray<TSharedPtr<FJsonValue>> TextFields;
	TArray<TSharedPtr<FJsonValue>> BoolFields;
	TArray<TSharedPtr<FJsonValue>> NumberFields;

	for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		const FString Name = Property->GetName();
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Value = ObjectProperty->GetObjectPropertyValue_InContainer(Actor);
			if (Value && Value->IsA<USoundBase>())
			{
				TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("property"), Name);
				Field->SetStringField(TEXT("asset"), GetObjectAssetPath(ObjectProperty, Actor));
				Field->SetNumberField(TEXT("duration_seconds"), CastChecked<USoundBase>(Value)->GetDuration());
				AudioFields.Add(MakeShared<FJsonValueObject>(Field));
			}
		}
		else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			if (HasAnyToken(Name, { TEXT("Subtitle"), TEXT("Speaker"), TEXT("Line"), TEXT("Text") }))
			{
				TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("property"), Name);
				Field->SetStringField(TEXT("value"), TextProperty->GetPropertyValue_InContainer(Actor).ToString());
				TextFields.Add(MakeShared<FJsonValueObject>(Field));
			}
		}
		else if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			if (HasAnyToken(Name, { TEXT("Subtitle"), TEXT("Speaker"), TEXT("Dispatch"), TEXT("Played") }))
			{
				TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("property"), Name);
				Field->SetBoolField(TEXT("value"), BoolProperty->GetPropertyValue_InContainer(Actor));
				BoolFields.Add(MakeShared<FJsonValueObject>(Field));
			}
		}
		else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsFloatingPoint() && HasAnyToken(Name, { TEXT("Duration"), TEXT("Delay"), TEXT("Fade") }))
			{
				TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("property"), Name);
				Field->SetNumberField(TEXT("value"), NumericProperty->GetFloatingPointPropertyValue(NumericProperty->ContainerPtrToValuePtr<void>(Actor)));
				NumberFields.Add(MakeShared<FJsonValueObject>(Field));
			}
		}
	}
	Data->SetArrayField(TEXT("audio_fields"), AudioFields);
	Data->SetArrayField(TEXT("text_fields"), TextFields);
	Data->SetArrayField(TEXT("bool_fields"), BoolFields);
	Data->SetArrayField(TEXT("number_fields"), NumberFields);
	return Data;
}

FMCPToolResult FMCPTool_NarrativeTrigger::ExecuteList(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	const FString ClassFilter = ExtractOptionalString(Params, TEXT("class_filter"));
	const FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	const int32 Limit = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("limit"), 100), 1, 1000);
	TArray<TSharedPtr<FJsonValue>> Triggers;
	int32 Total = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsNarrativeTrigger(Actor, ClassFilter)) continue;
		if (!NameFilter.IsEmpty() && !Actor->GetName().Contains(NameFilter, ESearchCase::IgnoreCase) && !Actor->GetActorLabel().Contains(NameFilter, ESearchCase::IgnoreCase)) continue;
		++Total;
		if (Triggers.Num() < Limit) Triggers.Add(MakeShared<FJsonValueObject>(BuildTriggerJson(Actor)));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("matching_triggers"), Total);
	Data->SetArrayField(TEXT("triggers"), Triggers);
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Listed placed narrative trigger instances"), Data);
	if (Total > Limit) Result.Warnings.Add(FString::Printf(TEXT("Showing the first %d of %d matching triggers. Use name_filter or a higher limit to narrow it."), Limit, Total));
	return Result;
}

FMCPToolResult FMCPTool_NarrativeTrigger::ExecuteRead(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString ActorName;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractActorName(Params, TEXT("actor_name"), ActorName, ParamError)) return ParamError.GetValue();
	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor) return ActorNotFoundError(ActorName);
	if (!IsNarrativeTrigger(Actor, FString())) return FMCPToolResult::Error(TEXT("That actor is not a recognised narrative trigger. Use list to find valid trigger instances."));
	return FMCPToolResult::Success(TEXT("Read placed narrative trigger instance"), BuildTriggerJson(Actor));
}

FMCPToolResult FMCPTool_NarrativeTrigger::ExecuteSubtitleAudit(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	const FString ClassFilter = ExtractOptionalString(Params, TEXT("class_filter"));
	const int32 Limit = FMath::Clamp(ExtractOptionalNumber<int32>(Params, TEXT("limit"), 100), 1, 1000);
	TArray<TSharedPtr<FJsonValue>> Findings;
	int32 Total = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsNarrativeTrigger(Actor, ClassFilter)) continue;
		++Total;
		const TSharedPtr<FJsonObject> Trigger = BuildTriggerJson(Actor);
		TArray<FString> Issues;
		const TArray<TSharedPtr<FJsonValue>>* Audio = nullptr;
		if (!Trigger->TryGetArrayField(TEXT("audio_fields"), Audio) || Audio->IsEmpty()) Issues.Add(TEXT("No USoundBase property is assigned or exposed."));
		bool bHasEnglish = false, bHasRomanian = false, bShowSubtitle = false;
		const TArray<TSharedPtr<FJsonValue>>* TextFields = nullptr;
		if (Trigger->TryGetArrayField(TEXT("text_fields"), TextFields))
		{
			for (const TSharedPtr<FJsonValue>& Value : *TextFields)
			{
				const TSharedPtr<FJsonObject>* Field = nullptr;
				if (!Value->TryGetObject(Field)) continue;
				FString Name, Text; (*Field)->TryGetStringField(TEXT("property"), Name); (*Field)->TryGetStringField(TEXT("value"), Text);
				if (Name.Contains(TEXT("English"), ESearchCase::IgnoreCase) && !Text.TrimStartAndEnd().IsEmpty()) bHasEnglish = true;
				if (Name.Contains(TEXT("Romanian"), ESearchCase::IgnoreCase) && !Text.TrimStartAndEnd().IsEmpty()) bHasRomanian = true;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* BoolFields = nullptr;
		if (Trigger->TryGetArrayField(TEXT("bool_fields"), BoolFields))
		{
			for (const TSharedPtr<FJsonValue>& Value : *BoolFields)
			{
				const TSharedPtr<FJsonObject>* Field = nullptr;
				if (!Value->TryGetObject(Field)) continue;
				FString Name; bool Enabled = false; (*Field)->TryGetStringField(TEXT("property"), Name); (*Field)->TryGetBoolField(TEXT("value"), Enabled);
				if (Name.Contains(TEXT("ShowSubtitle"), ESearchCase::IgnoreCase) || Name.Contains(TEXT("bShowSubtitle"), ESearchCase::IgnoreCase)) bShowSubtitle = Enabled;
			}
		}
		if (!bHasEnglish) Issues.Add(TEXT("English subtitle is empty or not exposed."));
		if (!bHasRomanian) Issues.Add(TEXT("Romanian subtitle is empty or not exposed."));
		if (!bShowSubtitle) Issues.Add(TEXT("Per-trigger Show Subtitle is disabled or not exposed."));
		if (!Issues.IsEmpty() && Findings.Num() < Limit)
		{
			Trigger->SetArrayField(TEXT("issues"), StringArrayToJsonArray(Issues));
			Findings.Add(MakeShared<FJsonValueObject>(Trigger));
		}
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("triggers_checked"), Total);
	Data->SetNumberField(TEXT("triggers_with_issues"), Findings.Num());
	Data->SetArrayField(TEXT("findings"), Findings);
	return FMCPToolResult::Success(TEXT("Completed placed-trigger subtitle audit"), Data);
}

FProperty* FMCPTool_NarrativeTrigger::FindPropertyByAliases(AActor* Actor, const TArray<FString>& Aliases) const
{
	if (!Actor) return nullptr;
	for (const FString& Alias : Aliases)
	{
		if (FProperty* Exact = Actor->GetClass()->FindPropertyByName(FName(*Alias))) return Exact;
	}
	for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
	{
		for (const FString& Alias : Aliases)
		{
			if (It->GetName().Equals(Alias, ESearchCase::IgnoreCase)) return *It;
		}
	}
	return nullptr;
}

bool FMCPTool_NarrativeTrigger::SetTextByAliases(AActor* Actor, const TArray<FString>& Aliases, const FString& Value, FString& OutProperty, FString& OutError) const
{
	FProperty* Property = FindPropertyByAliases(Actor, Aliases);
	FTextProperty* TextProperty = CastField<FTextProperty>(Property);
	if (!TextProperty) { OutError = FString::Printf(TEXT("No compatible text property found. Expected one of: %s"), *FString::Join(Aliases, TEXT(", "))); return false; }
	TextProperty->SetPropertyValue_InContainer(Actor, FText::FromString(Value));
	OutProperty = Property->GetName(); return true;
}

bool FMCPTool_NarrativeTrigger::SetBoolByAliases(AActor* Actor, const TArray<FString>& Aliases, bool Value, FString& OutProperty, FString& OutError) const
{
	FProperty* Property = FindPropertyByAliases(Actor, Aliases);
	FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property);
	if (!BoolProperty) { OutError = FString::Printf(TEXT("No compatible boolean property found. Expected one of: %s"), *FString::Join(Aliases, TEXT(", "))); return false; }
	BoolProperty->SetPropertyValue_InContainer(Actor, Value);
	OutProperty = Property->GetName(); return true;
}

bool FMCPTool_NarrativeTrigger::SetNumberByAliases(AActor* Actor, const TArray<FString>& Aliases, double Value, FString& OutProperty, FString& OutError) const
{
	FProperty* Property = FindPropertyByAliases(Actor, Aliases);
	FNumericProperty* NumberProperty = CastField<FNumericProperty>(Property);
	if (!NumberProperty || !NumberProperty->IsFloatingPoint()) { OutError = FString::Printf(TEXT("No compatible numeric property found. Expected one of: %s"), *FString::Join(Aliases, TEXT(", "))); return false; }
	NumberProperty->SetFloatingPointPropertyValue(NumberProperty->ContainerPtrToValuePtr<void>(Actor), Value);
	OutProperty = Property->GetName(); return true;
}

FMCPToolResult FMCPTool_NarrativeTrigger::ExecuteUpdateSubtitles(const TSharedRef<FJsonObject>& Params)
{
	UWorld* World = nullptr;
	if (auto Error = ValidateEditorContext(World)) return Error.GetValue();
	FString ActorName;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractActorName(Params, TEXT("actor_name"), ActorName, ParamError)) return ParamError.GetValue();
	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor) return ActorNotFoundError(ActorName);
	if (!IsNarrativeTrigger(Actor, FString())) return FMCPToolResult::Error(TEXT("That actor is not a recognised narrative trigger."));

	Actor->Modify();
	TArray<FString> Updated;
	TArray<FString> Warnings;
	FString Property, Error;
	FString Value;
	if (Params->TryGetStringField(TEXT("english"), Value)) { if (SetTextByAliases(Actor, { TEXT("SubtitleEnglish"), TEXT("Subtitle_English") }, Value, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	if (Params->TryGetStringField(TEXT("romanian"), Value)) { if (SetTextByAliases(Actor, { TEXT("SubtitleRomanian"), TEXT("Subtitle_Romanian") }, Value, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	if (Params->TryGetStringField(TEXT("speaker"), Value)) { if (SetTextByAliases(Actor, { TEXT("SubtitleSpeaker"), TEXT("Subtitle_Speaker") }, Value, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	bool BoolValue = false;
	if (Params->TryGetBoolField(TEXT("show_subtitle"), BoolValue)) { if (SetBoolByAliases(Actor, { TEXT("ShowSubtitle"), TEXT("bShowSubtitle"), TEXT("Show_Subtitle") }, BoolValue, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	if (Params->TryGetBoolField(TEXT("show_speaker"), BoolValue)) { if (SetBoolByAliases(Actor, { TEXT("ShowSpeaker"), TEXT("bShowSpeaker"), TEXT("Show_Speaker") }, BoolValue, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	double NumberValue = 0.0;
	if (Params->TryGetNumberField(TEXT("duration"), NumberValue)) { if (SetNumberByAliases(Actor, { TEXT("SubtitleDuration"), TEXT("Subtitle_Duration") }, NumberValue, Property, Error)) Updated.Add(Property); else Warnings.Add(Error); }
	if (Updated.IsEmpty()) return FMCPToolResult::Error(TEXT("No subtitle fields were supplied or no matching properties were found on the selected trigger."));
	Actor->MarkPackageDirty();
	MarkWorldDirty(World);
	TSharedPtr<FJsonObject> Data = BuildTriggerJson(Actor);
	Data->SetArrayField(TEXT("updated_properties"), StringArrayToJsonArray(Updated));
	FMCPToolResult Result = FMCPToolResult::Success(TEXT("Updated only the selected placed narrative trigger instance"), Data);
	Result.Warnings.Append(Warnings);
	return Result;
}
