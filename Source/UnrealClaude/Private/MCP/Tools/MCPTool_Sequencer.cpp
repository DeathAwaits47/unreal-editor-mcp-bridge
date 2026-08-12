// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Sequencer.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Animation/AnimSequenceBase.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sound/SoundBase.h"
#include "Misc/PackageName.h"
#include "ControlRig.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Rigs/RigHierarchy.h"

namespace
{
	ULevelSequence* LoadSequence(const FString& AssetPath, FString& OutError)
	{
		FString ObjectPath = AssetPath;
		ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		if (!Sequence && AssetPath.StartsWith(TEXT("/Game/")) && !AssetPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
			Sequence = LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		}
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("Could not load Level Sequence '%s'"), *AssetPath);
		}
		return Sequence;
	}

	bool ParseBindingGuid(const FString& BindingId, FGuid& OutGuid, FString& OutError)
	{
		if (!FGuid::Parse(BindingId, OutGuid))
		{
			OutError = FString::Printf(TEXT("Invalid binding_id '%s'. Use the ID returned by sequencer inspect or bind_actor."), *BindingId);
			return false;
		}
		return true;
	}

	void SaveSequence(ULevelSequence* Sequence)
	{
		Sequence->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(Sequence, false);
	}

	UMovieScene3DTransformSection* GetOrCreateTransformSection(UMovieScene3DTransformTrack* Track, FFrameNumber Frame)
	{
		for (UMovieSceneSection* Existing : Track->GetAllSections())
		{
			if (Existing && Existing->GetRange().Contains(Frame))
			{
				if (UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(Existing))
				{
					return TransformSection;
				}
			}
		}

		UMovieScene3DTransformSection* NewSection = Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
		if (NewSection)
		{
			NewSection->SetRange(TRange<FFrameNumber>::All());
			Track->AddSection(*NewSection);
		}
		return NewSection;
	}

	bool AddDoubleKey(UMovieScene3DTransformSection* Section, const TCHAR* ChannelName, FFrameNumber Frame, double Value, FString& OutError)
	{
		TMovieSceneChannelHandle<FMovieSceneDoubleChannel> Channel =
			Section->GetChannelProxy().GetChannelByName<FMovieSceneDoubleChannel>(FName(ChannelName));
		if (!Channel.Get())
		{
			OutError = FString::Printf(TEXT("Transform channel '%s' could not be found"), ChannelName);
			return false;
		}
		AddKeyToChannel(Channel.Get(), Frame, Value, EMovieSceneKeyInterpolation::Auto);
		return true;
	}
}

FMCPToolInfo FMCPTool_Sequencer::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("sequencer");
	Info.Description = TEXT(
		"Edit Level Sequences without using the desktop UI. Operations: inspect, bind_actor, set_playback_range, set_transform_key, add_animation_clip, add_audio_clip, inspect_control_rigs, set_control_rig_transform_key. "
		"Use this for actor and camera blocking, key placement, inserting authored animation clips, sequencing existing sound assets, and keying existing Control Rig hand/body controls. "
		"Control Rig operations enumerate the actual controls first and never guess names.");
	Info.Parameters = {
		FMCPToolParameter(TEXT("operation"), TEXT("string"), TEXT("inspect, bind_actor, set_playback_range, set_transform_key, add_animation_clip, add_audio_clip, inspect_control_rigs, or set_control_rig_transform_key"), true),
		FMCPToolParameter(TEXT("sequence_path"), TEXT("string"), TEXT("Level Sequence asset path, e.g. /Game/Cinematics/LS_Intro"), true),
		FMCPToolParameter(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label for bind_actor"), false),
		FMCPToolParameter(TEXT("binding_id"), TEXT("string"), TEXT("Binding GUID returned by inspect or bind_actor"), false),
		FMCPToolParameter(TEXT("frame"), TEXT("number"), TEXT("Frame for a key or animation clip"), false),
		FMCPToolParameter(TEXT("start_frame"), TEXT("number"), TEXT("Playback start frame"), false),
		FMCPToolParameter(TEXT("duration_frames"), TEXT("number"), TEXT("Playback duration in frames"), false),
		FMCPToolParameter(TEXT("location"), TEXT("object"), TEXT("Transform location: {x,y,z}"), false),
		FMCPToolParameter(TEXT("rotation"), TEXT("object"), TEXT("Transform rotation: {pitch,yaw,roll}"), false),
		FMCPToolParameter(TEXT("scale"), TEXT("object"), TEXT("Transform scale: {x,y,z}"), false),
		FMCPToolParameter(TEXT("animation_path"), TEXT("string"), TEXT("AnimSequence asset path for add_animation_clip"), false),
		FMCPToolParameter(TEXT("sound_path"), TEXT("string"), TEXT("Sound asset path for add_audio_clip"), false),
		FMCPToolParameter(TEXT("control_rig_index"), TEXT("number"), TEXT("Index returned by inspect_control_rigs"), false),
		FMCPToolParameter(TEXT("control_name"), TEXT("string"), TEXT("Exact Control Rig control name returned by inspect_control_rigs"), false)
	};
	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

FMCPToolResult FMCPTool_Sequencer::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, ParamError))
	{
		return ParamError.GetValue();
	}

	if (Operation.Equals(TEXT("inspect"), ESearchCase::IgnoreCase)) return ExecuteInspect(Params);
	if (Operation.Equals(TEXT("bind_actor"), ESearchCase::IgnoreCase)) return ExecuteBindActor(Params);
	if (Operation.Equals(TEXT("set_playback_range"), ESearchCase::IgnoreCase)) return ExecuteSetPlaybackRange(Params);
	if (Operation.Equals(TEXT("set_transform_key"), ESearchCase::IgnoreCase)) return ExecuteSetTransformKey(Params);
	if (Operation.Equals(TEXT("add_animation_clip"), ESearchCase::IgnoreCase)) return ExecuteAddAnimationClip(Params);
	if (Operation.Equals(TEXT("add_audio_clip"), ESearchCase::IgnoreCase)) return ExecuteAddAudioClip(Params);
	if (Operation.Equals(TEXT("inspect_control_rigs"), ESearchCase::IgnoreCase)) return ExecuteInspectControlRigs(Params);
	if (Operation.Equals(TEXT("set_control_rig_transform_key"), ESearchCase::IgnoreCase)) return ExecuteSetControlRigTransformKey(Params);

	return FMCPToolResult::Error(FString::Printf(TEXT("Unknown sequencer operation '%s'"), *Operation));
}

FMCPToolResult FMCPTool_Sequencer::ExecuteInspect(const TSharedRef<FJsonObject>& Params)
{
	FString Path; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError)) return ParamError.GetValue();
	FString Error; ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence) return FMCPToolResult::Error(Error);

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	TArray<TSharedPtr<FJsonValue>> Bindings;
	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
		BindingJson->SetStringField(TEXT("binding_id"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphens));
		BindingJson->SetStringField(TEXT("name"), Binding.GetName());
		TArray<TSharedPtr<FJsonValue>> Tracks;
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			TSharedPtr<FJsonObject> TrackJson = MakeShared<FJsonObject>();
			TrackJson->SetStringField(TEXT("class"), Track->GetClass()->GetName());
			TrackJson->SetNumberField(TEXT("sections"), Track->GetAllSections().Num());
			Tracks.Add(MakeShared<FJsonValueObject>(TrackJson));
		}
		BindingJson->SetArrayField(TEXT("tracks"), Tracks);
		Bindings.Add(MakeShared<FJsonValueObject>(BindingJson));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sequence"), Sequence->GetPathName());
	Data->SetNumberField(TEXT("playback_start"), MovieScene->GetPlaybackRange().GetLowerBoundValue().Value);
	Data->SetNumberField(TEXT("playback_duration"), MovieScene->GetPlaybackRange().Size<FFrameNumber>().Value);
	Data->SetArrayField(TEXT("bindings"), Bindings);
	return FMCPToolResult::Success(TEXT("Inspected Level Sequence"), Data);
}

FMCPToolResult FMCPTool_Sequencer::ExecuteBindActor(const TSharedRef<FJsonObject>& Params)
{
	FString Path, ActorName; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError) || !ExtractActorName(Params, TEXT("actor_name"), ActorName, ParamError)) return ParamError.GetValue();
	FString Error; ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence) return FMCPToolResult::Error(Error);
	UWorld* World = nullptr; if (auto ContextError = ValidateEditorContext(World)) return ContextError.GetValue();
	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor) return ActorNotFoundError(ActorName);

	FGuid Binding = Sequence->FindBindingFromObject(Actor, World);
	if (!Binding.IsValid())
	{
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		Sequence->Modify(); MovieScene->Modify();
		Binding = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
		Sequence->BindPossessableObject(Binding, *Actor, World);
		SaveSequence(Sequence);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("binding_id"), Binding.ToString(EGuidFormats::DigitsWithHyphens));
	Data->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	return FMCPToolResult::Success(TEXT("Actor bound to Level Sequence"), Data);
}

FMCPToolResult FMCPTool_Sequencer::ExecuteSetPlaybackRange(const TSharedRef<FJsonObject>& Params)
{
	FString Path; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError)) return ParamError.GetValue();
	const int32 Start = ExtractOptionalNumber<int32>(Params, TEXT("start_frame"), 0);
	const int32 Duration = ExtractOptionalNumber<int32>(Params, TEXT("duration_frames"), 0);
	if (Duration <= 0) return FMCPToolResult::Error(TEXT("duration_frames must be greater than zero"));
	FString Error; ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence) return FMCPToolResult::Error(Error);
	Sequence->GetMovieScene()->SetPlaybackRange(FFrameNumber(Start), Duration);
	SaveSequence(Sequence);
	return FMCPToolResult::Success(TEXT("Updated Level Sequence playback range"));
}

FMCPToolResult FMCPTool_Sequencer::ExecuteSetTransformKey(const TSharedRef<FJsonObject>& Params)
{
	FString Path, BindingId; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError) || !ExtractRequiredString(Params, TEXT("binding_id"), BindingId, ParamError)) return ParamError.GetValue();
	FGuid Binding; FString Error; if (!ParseBindingGuid(BindingId, Binding, Error)) return FMCPToolResult::Error(Error);
	const FFrameNumber Frame(ExtractOptionalNumber<int32>(Params, TEXT("frame"), 0));
	const bool bLocation = HasVectorParam(Params, TEXT("location"));
	const bool bRotation = Params->HasField(TEXT("rotation"));
	const bool bScale = HasVectorParam(Params, TEXT("scale"));
	if (!bLocation && !bRotation && !bScale) return FMCPToolResult::Error(TEXT("Provide location, rotation, or scale to set_transform_key"));
	ULevelSequence* Sequence = LoadSequence(Path, Error); if (!Sequence) return FMCPToolResult::Error(Error);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(Binding);
	if (!Track) Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Binding);
	if (!Track) return FMCPToolResult::Error(TEXT("Could not create Transform track for binding"));
	UMovieScene3DTransformSection* Section = GetOrCreateTransformSection(Track, Frame);
	if (!Section) return FMCPToolResult::Error(TEXT("Could not create Transform section"));
	if (bLocation)
	{
		const FVector V = ExtractVectorParam(Params, TEXT("location"));
		if (!AddDoubleKey(Section, TEXT("Location.X"), Frame, V.X, Error) || !AddDoubleKey(Section, TEXT("Location.Y"), Frame, V.Y, Error) || !AddDoubleKey(Section, TEXT("Location.Z"), Frame, V.Z, Error)) return FMCPToolResult::Error(Error);
	}
	if (bRotation)
	{
		const FRotator R = ExtractRotatorParam(Params, TEXT("rotation"));
		if (!AddDoubleKey(Section, TEXT("Rotation.X"), Frame, R.Roll, Error) || !AddDoubleKey(Section, TEXT("Rotation.Y"), Frame, R.Pitch, Error) || !AddDoubleKey(Section, TEXT("Rotation.Z"), Frame, R.Yaw, Error)) return FMCPToolResult::Error(Error);
	}
	if (bScale)
	{
		const FVector V = ExtractScaleParam(Params, TEXT("scale"));
		if (!AddDoubleKey(Section, TEXT("Scale.X"), Frame, V.X, Error) || !AddDoubleKey(Section, TEXT("Scale.Y"), Frame, V.Y, Error) || !AddDoubleKey(Section, TEXT("Scale.Z"), Frame, V.Z, Error)) return FMCPToolResult::Error(Error);
	}
	SaveSequence(Sequence);
	return FMCPToolResult::Success(TEXT("Added transform key(s) to Level Sequence"));
}

FMCPToolResult FMCPTool_Sequencer::ExecuteAddAnimationClip(const TSharedRef<FJsonObject>& Params)
{
	FString Path, BindingId, AnimationPath; TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError) || !ExtractRequiredString(Params, TEXT("binding_id"), BindingId, ParamError) || !ExtractRequiredString(Params, TEXT("animation_path"), AnimationPath, ParamError)) return ParamError.GetValue();
	FGuid Binding; FString Error; if (!ParseBindingGuid(BindingId, Binding, Error)) return FMCPToolResult::Error(Error);
	ULevelSequence* Sequence = LoadSequence(Path, Error); if (!Sequence) return FMCPToolResult::Error(Error);
	UAnimSequenceBase* Animation = LoadObject<UAnimSequenceBase>(nullptr, *AnimationPath);
	if (!Animation && AnimationPath.StartsWith(TEXT("/Game/")) && !AnimationPath.Contains(TEXT(".")))
	{
		const FString Name = FPackageName::GetLongPackageAssetName(AnimationPath);
		Animation = LoadObject<UAnimSequenceBase>(nullptr, *FString::Printf(TEXT("%s.%s"), *AnimationPath, *Name));
	}
	if (!Animation) return FMCPToolResult::Error(FString::Printf(TEXT("Could not load animation '%s'"), *AnimationPath));
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	UMovieSceneSkeletalAnimationTrack* Track = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(Binding);
	if (!Track) Track = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Binding);
	if (!Track) return FMCPToolResult::Error(TEXT("Could not create skeletal animation track for binding"));
	Track->AddNewAnimation(FFrameNumber(ExtractOptionalNumber<int32>(Params, TEXT("frame"), 0)), Animation);
	SaveSequence(Sequence);
	return FMCPToolResult::Success(TEXT("Added animation clip to Level Sequence"));
}

FMCPToolResult FMCPTool_Sequencer::ExecuteAddAudioClip(const TSharedRef<FJsonObject>& Params)
{
	FString Path, SoundPath;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError) ||
		!ExtractRequiredString(Params, TEXT("sound_path"), SoundPath, ParamError))
	{
		return ParamError.GetValue();
	}

	FString Error;
	ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence)
	{
		return FMCPToolResult::Error(Error);
	}

	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *SoundPath);
	if (!Sound && SoundPath.StartsWith(TEXT("/Game/")) && !SoundPath.Contains(TEXT(".")))
	{
		const FString Name = FPackageName::GetLongPackageAssetName(SoundPath);
		Sound = LoadObject<USoundBase>(nullptr, *FString::Printf(TEXT("%s.%s"), *SoundPath, *Name));
	}
	if (!Sound)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load sound '%s'"), *SoundPath));
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	UMovieSceneAudioTrack* Track = MovieScene->FindMasterTrack<UMovieSceneAudioTrack>();
	if (!Track)
	{
		Track = MovieScene->AddMasterTrack<UMovieSceneAudioTrack>();
	}
	if (!Track)
	{
		return FMCPToolResult::Error(TEXT("Could not create a master Audio track"));
	}

	Track->AddNewSound(FFrameNumber(ExtractOptionalNumber<int32>(Params, TEXT("frame"), 0)), Sound);
	SaveSequence(Sequence);
	return FMCPToolResult::Success(TEXT("Added audio clip to Level Sequence"));
}

FMCPToolResult FMCPTool_Sequencer::ExecuteInspectControlRigs(const TSharedRef<FJsonObject>& Params)
{
	FString Path;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError))
	{
		return ParamError.GetValue();
	}

	FString Error;
	ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence)
	{
		return FMCPToolResult::Error(Error);
	}

	TArray<FControlRigSequencerBindingProxy> ControlRigs = UControlRigSequencerEditorLibrary::GetControlRigs(Sequence);
	TArray<TSharedPtr<FJsonValue>> RigEntries;
	for (int32 RigIndex = 0; RigIndex < ControlRigs.Num(); ++RigIndex)
	{
		UControlRig* Rig = ControlRigs[RigIndex].ControlRig;
		if (!Rig || !Rig->GetHierarchy())
		{
			continue;
		}

		TSharedPtr<FJsonObject> RigJson = MakeShared<FJsonObject>();
		RigJson->SetNumberField(TEXT("control_rig_index"), RigIndex);
		RigJson->SetStringField(TEXT("rig_name"), Rig->GetName());
		TArray<TSharedPtr<FJsonValue>> Controls;
		for (const FRigElementKey& Key : Rig->GetHierarchy()->GetControlKeys())
		{
			Controls.Add(MakeShared<FJsonValueString>(Key.Name.ToString()));
		}
		RigJson->SetArrayField(TEXT("controls"), Controls);
		RigEntries.Add(MakeShared<FJsonValueObject>(RigJson));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("control_rigs"), RigEntries);
	return FMCPToolResult::Success(TEXT("Inspected Control Rig tracks in Level Sequence"), Data);
}

FMCPToolResult FMCPTool_Sequencer::ExecuteSetControlRigTransformKey(const TSharedRef<FJsonObject>& Params)
{
	FString Path, ControlName;
	TOptional<FMCPToolResult> ParamError;
	if (!ExtractRequiredString(Params, TEXT("sequence_path"), Path, ParamError) ||
		!ExtractRequiredString(Params, TEXT("control_name"), ControlName, ParamError))
	{
		return ParamError.GetValue();
	}

	const int32 RigIndex = ExtractOptionalNumber<int32>(Params, TEXT("control_rig_index"), INDEX_NONE);
	if (RigIndex == INDEX_NONE)
	{
		return FMCPToolResult::Error(TEXT("control_rig_index is required. Call inspect_control_rigs first."));
	}
	if (!HasVectorParam(Params, TEXT("location")) && !Params->HasField(TEXT("rotation")) && !HasVectorParam(Params, TEXT("scale")))
	{
		return FMCPToolResult::Error(TEXT("Provide location, rotation, or scale for the Control Rig key."));
	}

	FString Error;
	ULevelSequence* Sequence = LoadSequence(Path, Error);
	if (!Sequence)
	{
		return FMCPToolResult::Error(Error);
	}

	TArray<FControlRigSequencerBindingProxy> ControlRigs = UControlRigSequencerEditorLibrary::GetControlRigs(Sequence);
	if (!ControlRigs.IsValidIndex(RigIndex) || !ControlRigs[RigIndex].ControlRig)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Control Rig index %d is not present in this Level Sequence. Call inspect_control_rigs again."), RigIndex));
	}

	UControlRig* Rig = ControlRigs[RigIndex].ControlRig;
	const FRigElementKey ControlKey(FName(*ControlName), ERigElementType::Control);
	if (!Rig->GetHierarchy() || !Rig->GetHierarchy()->Contains(ControlKey))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Control '%s' does not exist on '%s'. Use inspect_control_rigs for exact names."), *ControlName, *Rig->GetName()));
	}

	const FFrameNumber Frame(ExtractOptionalNumber<int32>(Params, TEXT("frame"), 0));
	const TArray<FFrameNumber> QueryFrames = { Frame };
	const TArray<FTransform> ExistingValues = UControlRigSequencerEditorLibrary::GetLocalControlRigTransforms(
		Sequence, Rig, FName(*ControlName), QueryFrames, EMovieSceneTimeUnit::DisplayRate);
	FTransform Value = ExistingValues.Num() == 1 ? ExistingValues[0] : FTransform::Identity;
	if (HasVectorParam(Params, TEXT("location")))
	{
		Value.SetLocation(ExtractVectorParam(Params, TEXT("location")));
	}
	if (Params->HasField(TEXT("rotation")))
	{
		Value.SetRotation(ExtractRotatorParam(Params, TEXT("rotation")).Quaternion());
	}
	if (HasVectorParam(Params, TEXT("scale")))
	{
		Value.SetScale3D(ExtractScaleParam(Params, TEXT("scale")));
	}
	UControlRigSequencerEditorLibrary::SetLocalControlRigTransform(
		Sequence, Rig, FName(*ControlName), Frame, Value, EMovieSceneTimeUnit::DisplayRate, true);
	SaveSequence(Sequence);
	return FMCPToolResult::Success(FString::Printf(TEXT("Added Control Rig transform key for '%s' at frame %d"), *ControlName, Frame.Value));
}
