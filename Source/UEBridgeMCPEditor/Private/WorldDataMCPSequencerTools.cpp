#include "WorldDataMCPSequencerTools.h"

#include "WorldDataMCPCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/FrameRate.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace WorldDataMCP
{
namespace SequencerTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
	}

	ULevelSequence* LoadSequence(const TSharedPtr<FJsonObject>& Args, FString& OutError)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			Args->TryGetStringField(TEXT("path"), AssetPath);
		}
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing 'assetPath'.");
			return nullptr;
		}
		ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(AssetPath));
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("LevelSequence '%s' not found."), *AssetPath);
		}
		return Sequence;
	}

	AActor* FindActorByLabelOrName(UWorld* World, const FString& Label)
	{
		if (!World || Label.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }
			if (Actor->GetActorLabel() == Label || Actor->GetName() == Label)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	// Find an existing possessable binding for the actor, or create + bind one. Returns invalid Guid on failure.
	FGuid FindOrAddActorBinding(ULevelSequence* Sequence, UMovieScene* MovieScene, UWorld* World, AActor* Actor, const FString& Label)
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
			if (P.GetName() == Label || P.GetName() == Actor->GetName())
			{
				return P.GetGuid();
			}
		}
		const FGuid Guid = MovieScene->AddPossessable(Label.IsEmpty() ? Actor->GetActorLabel() : Label, Actor->GetClass());
		Sequence->BindPossessableObject(Guid, *Actor, World);
		return Guid;
	}

	FVector ReadVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FVector& Fallback, bool& bPresent,
		const TCHAR* KX = TEXT("x"), const TCHAR* KY = TEXT("y"), const TCHAR* KZ = TEXT("z"))
	{
		const TSharedPtr<FJsonObject>* VObj = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, VObj) && VObj && (*VObj).IsValid())
		{
			bPresent = true;
			double X = Fallback.X, Y = Fallback.Y, Z = Fallback.Z;
			(*VObj)->TryGetNumberField(KX, X);
			(*VObj)->TryGetNumberField(KY, Y);
			(*VObj)->TryGetNumberField(KZ, Z);
			return FVector(X, Y, Z);
		}
		bPresent = false;
		return Fallback;
	}

	// Add a key to a named double channel of the transform section, honoring interp mode.
	void AddDoubleKey(FMovieSceneChannelProxy& Proxy, FName ChannelName, FFrameNumber Frame, double Value, const FString& Interp)
	{
		FMovieSceneDoubleChannel* Channel = Proxy.GetChannelByName<FMovieSceneDoubleChannel>(ChannelName).Get();
		if (!Channel)
		{
			return;
		}
		if (Interp.Equals(TEXT("cubic"), ESearchCase::IgnoreCase))
		{
			Channel->AddCubicKey(Frame, Value);
		}
		else if (Interp.Equals(TEXT("constant"), ESearchCase::IgnoreCase))
		{
			Channel->AddConstantKey(Frame, Value);
		}
		else
		{
			Channel->AddLinearKey(Frame, Value);
		}
	}

	// ---- tools ---------------------------------------------------------------------------

	FString CreateLevelSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		FString PackagePath = TEXT("/Game/Cinematics");
		Args->TryGetStringField(TEXT("packagePath"), PackagePath);

		const FString PackageFullPath = PackagePath / Name;
		if (UEditorAssetLibrary::DoesAssetExist(PackageFullPath))
		{
			return ErrorJson(FString::Printf(TEXT("Asset already exists: %s"), *PackageFullPath));
		}
		UPackage* Package = CreatePackage(*PackageFullPath);
		if (!Package)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create package: %s"), *PackageFullPath));
		}
		ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (!Sequence)
		{
			return ErrorJson(TEXT("Failed to create ULevelSequence."));
		}
		Sequence->Initialize();

		FAssetRegistryModule::AssetCreated(Sequence);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(PackageFullPath, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), Sequence->GetPathName());
		return SuccessJson(Result);
	}

	FString AddActorToSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		ULevelSequence* Sequence = LoadSequence(Args, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			return ErrorJson(TEXT("LevelSequence has no MovieScene."));
		}
		UWorld* World = GetEditorWorld();
		FString ActorLabel;
		Args->TryGetStringField(TEXT("actorLabel"), ActorLabel);
		AActor* Actor = FindActorByLabelOrName(World, ActorLabel);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found in editor world."), *ActorLabel));
		}
		const FGuid Guid = FindOrAddActorBinding(Sequence, MovieScene, World, Actor, ActorLabel);
		if (!Guid.IsValid())
		{
			return ErrorJson(TEXT("Failed to create actor binding."));
		}
		Sequence->GetOutermost()->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(Sequence, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Sequence->GetPathName());
		Result->SetStringField(TEXT("actorLabel"), ActorLabel);
		Result->SetStringField(TEXT("bindingGuid"), Guid.ToString());
		return SuccessJson(Result);
	}

	FString AddTransformKeys(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		ULevelSequence* Sequence = LoadSequence(Args, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			return ErrorJson(TEXT("LevelSequence has no MovieScene."));
		}
		UWorld* World = GetEditorWorld();
		FString ActorLabel;
		Args->TryGetStringField(TEXT("actorLabel"), ActorLabel);
		AActor* Actor = FindActorByLabelOrName(World, ActorLabel);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found in editor world."), *ActorLabel));
		}
		const FGuid Guid = FindOrAddActorBinding(Sequence, MovieScene, World, Actor, ActorLabel);
		if (!Guid.IsValid())
		{
			return ErrorJson(TEXT("Failed to create actor binding."));
		}

		const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("keys"), KeysArr) || !KeysArr || KeysArr->Num() == 0)
		{
			return ErrorJson(TEXT("Missing non-empty 'keys' array ([{time, location?, rotation?, scale?}])."));
		}
		FString Interp = TEXT("linear");
		Args->TryGetStringField(TEXT("interpolation"), Interp);

		// Find or create the transform track + a single section.
		UMovieScene3DTransformTrack* Track = Cast<UMovieScene3DTransformTrack>(
			MovieScene->FindTrack(UMovieScene3DTransformTrack::StaticClass(), Guid));
		if (!Track)
		{
			Track = Cast<UMovieScene3DTransformTrack>(MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), Guid));
		}
		if (!Track)
		{
			return ErrorJson(TEXT("Failed to add a 3D transform track."));
		}
		UMovieScene3DTransformSection* Section = nullptr;
		for (UMovieSceneSection* S : Track->GetAllSections())
		{
			Section = Cast<UMovieScene3DTransformSection>(S);
			if (Section) { break; }
		}
		bool bNewSection = false;
		if (!Section)
		{
			Section = Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
			if (!Section)
			{
				return ErrorJson(TEXT("Failed to create a 3D transform section."));
			}
			Track->AddSection(*Section);
			bNewSection = true;
		}

		const FFrameRate Tick = MovieScene->GetTickResolution();
		FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

		int32 KeysWritten = 0;
		FFrameNumber MinFrame(TNumericLimits<int32>::Max());
		FFrameNumber MaxFrame(TNumericLimits<int32>::Min());

		for (const TSharedPtr<FJsonValue>& KV : *KeysArr)
		{
			const TSharedPtr<FJsonObject>* KObj = nullptr;
			if (!KV->TryGetObject(KObj) || !(*KObj).IsValid()) { continue; }

			double TimeSeconds = 0.0;
			(*KObj)->TryGetNumberField(TEXT("time"), TimeSeconds);
			const FFrameNumber Frame = Tick.AsFrameNumber(TimeSeconds);
			MinFrame = FMath::Min(MinFrame, Frame);
			MaxFrame = FMath::Max(MaxFrame, Frame);

			bool bHas = false;
			const FVector Loc = ReadVec(*KObj, TEXT("location"), FVector::ZeroVector, bHas);
			if (bHas)
			{
				AddDoubleKey(Proxy, TEXT("Location.X"), Frame, Loc.X, Interp);
				AddDoubleKey(Proxy, TEXT("Location.Y"), Frame, Loc.Y, Interp);
				AddDoubleKey(Proxy, TEXT("Location.Z"), Frame, Loc.Z, Interp);
				++KeysWritten;
			}
			// rotation as {pitch,yaw,roll}; channels are Rotation.X=Roll, .Y=Pitch, .Z=Yaw.
			const FVector Rot = ReadVec(*KObj, TEXT("rotation"), FVector::ZeroVector, bHas, TEXT("pitch"), TEXT("yaw"), TEXT("roll"));
			if (bHas)
			{
				const double Pitch = Rot.X, Yaw = Rot.Y, Roll = Rot.Z;
				AddDoubleKey(Proxy, TEXT("Rotation.X"), Frame, Roll, Interp);
				AddDoubleKey(Proxy, TEXT("Rotation.Y"), Frame, Pitch, Interp);
				AddDoubleKey(Proxy, TEXT("Rotation.Z"), Frame, Yaw, Interp);
				++KeysWritten;
			}
			const FVector Scale = ReadVec(*KObj, TEXT("scale"), FVector::OneVector, bHas);
			if (bHas)
			{
				AddDoubleKey(Proxy, TEXT("Scale.X"), Frame, Scale.X, Interp);
				AddDoubleKey(Proxy, TEXT("Scale.Y"), Frame, Scale.Y, Interp);
				AddDoubleKey(Proxy, TEXT("Scale.Z"), Frame, Scale.Z, Interp);
				++KeysWritten;
			}
		}

		if (KeysWritten == 0)
		{
			return ErrorJson(TEXT("No keys written: each key needs at least one of location/rotation/scale."));
		}

		// Size the section to cover its keys, and extend the sequence playback range.
		if (MaxFrame < MinFrame) { MaxFrame = MinFrame; }
		if (MaxFrame == MinFrame) { MaxFrame = MinFrame + 1; }
		Section->SetRange(TRange<FFrameNumber>(MinFrame, MaxFrame));

		TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
		const FFrameNumber PlayLo = Playback.HasLowerBound() ? FMath::Min(Playback.GetLowerBoundValue(), MinFrame) : MinFrame;
		const FFrameNumber PlayHi = Playback.HasUpperBound() ? FMath::Max(Playback.GetUpperBoundValue(), MaxFrame) : MaxFrame;
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(PlayLo, PlayHi));

		Sequence->GetOutermost()->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(Sequence, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Sequence->GetPathName());
		Result->SetStringField(TEXT("actorLabel"), ActorLabel);
		Result->SetStringField(TEXT("bindingGuid"), Guid.ToString());
		Result->SetNumberField(TEXT("keyframesWritten"), KeysWritten);
		Result->SetBoolField(TEXT("createdSection"), bNewSection);
		Result->SetNumberField(TEXT("startFrame"), MinFrame.Value);
		Result->SetNumberField(TEXT("endFrame"), MaxFrame.Value);
		return SuccessJson(Result);
	}

	FString SetSequencePlaybackRange(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		ULevelSequence* Sequence = LoadSequence(Args, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			return ErrorJson(TEXT("LevelSequence has no MovieScene."));
		}
		const FFrameRate Tick = MovieScene->GetTickResolution();

		// Accept seconds (startSeconds/endSeconds) or raw frames (startFrame/endFrame).
		// NB: read BOTH fields unconditionally — a `||` here short-circuits and skips endSeconds.
		FFrameNumber StartFrame, EndFrame;
		double StartSeconds = 0, EndSeconds = 0;
		const bool bHasStartSec = Args->TryGetNumberField(TEXT("startSeconds"), StartSeconds);
		const bool bHasEndSec = Args->TryGetNumberField(TEXT("endSeconds"), EndSeconds);
		if (bHasStartSec || bHasEndSec)
		{
			StartFrame = Tick.AsFrameNumber(StartSeconds);
			EndFrame = Tick.AsFrameNumber(EndSeconds);
		}
		else
		{
			double SF = 0, EF = 0;
			Args->TryGetNumberField(TEXT("startFrame"), SF);
			Args->TryGetNumberField(TEXT("endFrame"), EF);
			StartFrame = FFrameNumber(static_cast<int32>(SF));
			EndFrame = FFrameNumber(static_cast<int32>(EF));
		}
		if (EndFrame <= StartFrame)
		{
			return ErrorJson(TEXT("End must be greater than start (provide startSeconds/endSeconds or startFrame/endFrame)."));
		}
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
		Sequence->GetOutermost()->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(Sequence, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Sequence->GetPathName());
		Result->SetNumberField(TEXT("startFrame"), StartFrame.Value);
		Result->SetNumberField(TEXT("endFrame"), EndFrame.Value);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_level_sequence","description":"Create a new empty Level Sequence asset (and Initialize its MovieScene).","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"packagePath":{"type":"string","description":"Folder, default /Game/Cinematics."}},"required":["name"]},"annotations":{"title":"Create Level Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_actor_to_sequence","description":"Possess a level actor (by label or name) in a Level Sequence, creating + binding the possessable. Idempotent. Returns the binding GUID.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"actorLabel":{"type":"string"}},"required":["assetPath","actorLabel"]},"annotations":{"title":"Add Actor To Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_transform_keys","description":"Keyframe an actor's transform in a Level Sequence. Possesses the actor if needed, ensures a 3D transform track+section, and writes keys. 'keys' = [{time:seconds, location?:{x,y,z}, rotation?:{pitch,yaw,roll}, scale?:{x,y,z}}]. Each key needs at least one of location/rotation/scale. interpolation: linear (default), cubic, constant. Extends the playback range to cover the keys. Use this to animate e.g. a directional light's rotation for a sunset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"actorLabel":{"type":"string"},"keys":{"type":"array"},"interpolation":{"type":"string"}},"required":["assetPath","actorLabel","keys"]},"annotations":{"title":"Add Transform Keys","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_sequence_playback_range","description":"Set a Level Sequence's playback range. Provide startSeconds/endSeconds OR startFrame/endFrame (tick-resolution frames).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"startSeconds":{"type":"number"},"endSeconds":{"type":"number"},"startFrame":{"type":"number"},"endFrame":{"type":"number"}},"required":["assetPath"]},"annotations":{"title":"Set Sequence Playback Range","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_level_sequence")) { OutResult = CreateLevelSequence(Args); return true; }
	if (ToolName == TEXT("add_actor_to_sequence")) { OutResult = AddActorToSequence(Args); return true; }
	if (ToolName == TEXT("add_transform_keys")) { OutResult = AddTransformKeys(Args); return true; }
	if (ToolName == TEXT("set_sequence_playback_range")) { OutResult = SetSequencePlaybackRange(Args); return true; }
	return false;
}
}
}
