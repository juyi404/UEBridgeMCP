#include "WorldDataMCPAIDirectorTools.h"

#include "AIDirectorJsonExportLibrary.h"
#include "AIDirectorSequenceAsset.h"
#include "AIDirectorSequencerLibrary.h"
#include "AIDirectorShotTypes.h"
#include "WorldDataMCPCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace WorldDataMCP
{
namespace AIDirectorTools
{
namespace
{
	bool TryGetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
	}

	bool TryGetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, FString& OutValue)
	{
		return TryGetString(Object, Key, OutValue) || TryGetString(Object, Alias, OutValue);
	}

	bool TryGetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& OutValue)
	{
		return Object.IsValid() && Object->TryGetNumberField(Key, OutValue);
	}

	bool TryGetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, double& OutValue)
	{
		return TryGetNumber(Object, Key, OutValue) || TryGetNumber(Object, Alias, OutValue);
	}

	bool TryGetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool& OutValue)
	{
		return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
	}

	bool TryGetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, bool& OutValue)
	{
		return TryGetBool(Object, Key, OutValue) || TryGetBool(Object, Alias, OutValue);
	}

	bool TryGetInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, int32& OutValue)
	{
		double Number = 0.0;
		if (!TryGetNumber(Object, Key, Alias, Number))
		{
			return false;
		}

		OutValue = FMath::RoundToInt(Number);
		return true;
	}

	TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Key, Field) && Field)
		{
			return *Field;
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias)
	{
		TSharedPtr<FJsonObject> Field = GetObjectField(Object, Key);
		return Field.IsValid() ? Field : GetObjectField(Object, Alias);
	}

	bool TryGetArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		return Object.IsValid() && Object->TryGetArrayField(Key, OutArray);
	}

	bool NormalizePackagePath(const FString& InPath, FString& OutPackageName, FString& OutAssetName, FString& OutError)
	{
		FString CleanPath = InPath;
		CleanPath.TrimStartAndEndInline();
		CleanPath.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (CleanPath.IsEmpty())
		{
			OutError = TEXT("Missing asset path.");
			return false;
		}

		if (!CleanPath.StartsWith(TEXT("/")))
		{
			CleanPath = TEXT("/Game/") + CleanPath;
		}

		if (CleanPath.Contains(TEXT(".")))
		{
			CleanPath = FPackageName::ObjectPathToPackageName(CleanPath);
		}

		OutPackageName = CleanPath;
		OutAssetName = FPackageName::GetShortName(OutPackageName);

		if (OutAssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(OutPackageName))
		{
			OutError = FString::Printf(TEXT("Invalid asset package path: %s"), *InPath);
			return false;
		}

		return true;
	}

	bool ResolveAssetPath(const TSharedPtr<FJsonObject>& Args, FString& OutPackageName, FString& OutAssetName, FString& OutError)
	{
		FString AssetPath;
		if (TryGetString(Args, TEXT("assetPath"), TEXT("path"), AssetPath))
		{
			return NormalizePackagePath(AssetPath, OutPackageName, OutAssetName, OutError);
		}

		FString Name;
		if (!TryGetString(Args, TEXT("name"), Name) || Name.IsEmpty())
		{
			OutError = TEXT("Missing 'assetPath' or 'name'.");
			return false;
		}

		FString PackagePath = TEXT("/Game/AIDirector");
		TryGetString(Args, TEXT("packagePath"), PackagePath);
		PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			PackagePath = TEXT("/Game/") + PackagePath;
		}

		return NormalizePackagePath(PackagePath / Name, OutPackageName, OutAssetName, OutError);
	}

	UAIDirectorSequenceAsset* LoadDirectorAsset(const TSharedPtr<FJsonObject>& Args, FString& OutPackageName, FString& OutError)
	{
		FString AssetName;
		if (!ResolveAssetPath(Args, OutPackageName, AssetName, OutError))
		{
			return nullptr;
		}

		UAIDirectorSequenceAsset* Sequence = Cast<UAIDirectorSequenceAsset>(UEditorAssetLibrary::LoadAsset(OutPackageName));
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("AI Director asset not found: %s"), *OutPackageName);
		}
		return Sequence;
	}

	UAIDirectorSequenceAsset* CreateDirectorAssetAtPath(
		const FString& PackageName,
		const FString& AssetName,
		int32 OutputFrameRate,
		FString& OutError)
	{
		if (UEditorAssetLibrary::DoesAssetExist(PackageName))
		{
			OutError = FString::Printf(TEXT("Asset already exists: %s"), *PackageName);
			return nullptr;
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
			return nullptr;
		}

		UAIDirectorSequenceAsset* Sequence = NewObject<UAIDirectorSequenceAsset>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Sequence)
		{
			OutError = TEXT("Failed to create UAIDirectorSequenceAsset.");
			return nullptr;
		}

		Sequence->OutputFrameRate = FMath::Clamp(OutputFrameRate, 1, 240);
		FAssetRegistryModule::AssetCreated(Sequence);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(PackageName, false);
		return Sequence;
	}

	bool SaveDirectorAsset(UAIDirectorSequenceAsset* Sequence)
	{
		if (!Sequence)
		{
			return false;
		}

		Sequence->MarkPackageDirty();
		return UEditorAssetLibrary::SaveLoadedAsset(Sequence, false);
	}

	template <typename TEnum>
	FString EnumToString(TEnum Value)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Value)) : FString::FromInt(static_cast<int32>(Value));
	}

	template <typename TEnum>
	bool TryParseEnum(const FString& RawValue, TEnum& OutValue, FString& OutError, const TCHAR* FieldName)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			OutError = FString::Printf(TEXT("Enum metadata missing for '%s'."), FieldName);
			return false;
		}

		FString Value = RawValue;
		Value.TrimStartAndEndInline();
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			const FString ShortName = Enum->GetNameStringByIndex(Index);
			const FString FullName = Enum->GetNameByIndex(Index).ToString();
			if (ShortName.Equals(Value, ESearchCase::IgnoreCase) || FullName.Equals(Value, ESearchCase::IgnoreCase))
			{
				OutValue = static_cast<TEnum>(Enum->GetValueByIndex(Index));
				return true;
			}
		}

		OutError = FString::Printf(TEXT("Invalid enum value '%s' for '%s'."), *RawValue, FieldName);
		return false;
	}

	template <typename TEnum>
	bool ApplyEnumField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, TEnum& OutValue, FString& OutError)
	{
		FString RawValue;
		if (!TryGetString(Object, Key, Alias, RawValue))
		{
			return true;
		}

		return TryParseEnum(RawValue, OutValue, OutError, Key);
	}

	bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, FVector& OutValue)
	{
		TSharedPtr<FJsonObject> VectorObject = GetObjectField(Object, Key, Alias);
		if (!VectorObject.IsValid())
		{
			return false;
		}

		double X = OutValue.X;
		double Y = OutValue.Y;
		double Z = OutValue.Z;
		TryGetNumber(VectorObject, TEXT("x"), X);
		TryGetNumber(VectorObject, TEXT("y"), Y);
		TryGetNumber(VectorObject, TEXT("z"), Z);
		OutValue = FVector(X, Y, Z);
		return true;
	}

	bool ReadVector2D(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const TCHAR* Alias, FVector2D& OutValue)
	{
		TSharedPtr<FJsonObject> VectorObject = GetObjectField(Object, Key, Alias);
		if (!VectorObject.IsValid())
		{
			return false;
		}

		double X = OutValue.X;
		double Y = OutValue.Y;
		TryGetNumber(VectorObject, TEXT("x"), X);
		TryGetNumber(VectorObject, TEXT("y"), Y);
		OutValue = FVector2D(X, Y);
		return true;
	}

	TSharedRef<FJsonObject> VectorToJson(const FVector& Vector)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedRef<FJsonObject> Vector2DToJson(const FVector2D& Vector)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		return Object;
	}

	bool ApplyShotJson(const TSharedPtr<FJsonObject>& ShotObject, FAIDirectorShot& Shot, FString& OutError)
	{
		if (!ShotObject.IsValid())
		{
			OutError = TEXT("Shot payload must be a JSON object.");
			return false;
		}

		FString TextValue;
		double NumberValue = 0.0;
		bool BoolValue = false;

		if (TryGetString(ShotObject, TEXT("shotName"), TEXT("name"), TextValue))
		{
			Shot.ShotName = FName(*TextValue);
		}
		if (TryGetBool(ShotObject, TEXT("enabled"), TEXT("bEnabled"), BoolValue))
		{
			Shot.bEnabled = BoolValue;
		}
		if (TryGetNumber(ShotObject, TEXT("durationSeconds"), TEXT("duration_seconds"), NumberValue))
		{
			Shot.DurationSeconds = FMath::Max(0.1f, static_cast<float>(NumberValue));
		}
		if (TryGetString(ShotObject, TEXT("notes"), TextValue))
		{
			Shot.Notes = TextValue;
		}

		if (TSharedPtr<FJsonObject> Subject = GetObjectField(ShotObject, TEXT("subject")))
		{
			if (!ApplyEnumField(Subject, TEXT("mode"), TEXT("subjectMode"), Shot.Subject.Mode, OutError))
			{
				return false;
			}
			if (TryGetString(Subject, TEXT("actor"), TEXT("actorPath"), TextValue))
			{
				Shot.Subject.Actor = TSoftObjectPtr<AActor>(FSoftObjectPath(TextValue));
			}
			if (TryGetString(Subject, TEXT("actorTag"), TEXT("actor_tag"), TextValue))
			{
				Shot.Subject.ActorTag = FName(*TextValue);
			}
			ReadVector(Subject, TEXT("worldLocation"), TEXT("world_location"), Shot.Subject.WorldLocation);
		}

		if (TSharedPtr<FJsonObject> Move = GetObjectField(ShotObject, TEXT("move")))
		{
			if (!ApplyEnumField(Move, TEXT("moveType"), TEXT("type"), Shot.Move.MoveType, OutError))
			{
				return false;
			}
			if (!ApplyEnumField(Move, TEXT("easing"), TEXT("ease"), Shot.Move.Easing, OutError))
			{
				return false;
			}
			if (TryGetNumber(Move, TEXT("startDistance"), TEXT("start_distance"), NumberValue)) { Shot.Move.StartDistance = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("endDistance"), TEXT("end_distance"), NumberValue)) { Shot.Move.EndDistance = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("orbitDegrees"), TEXT("orbit_degrees"), NumberValue)) { Shot.Move.OrbitDegrees = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("panDegrees"), TEXT("pan_degrees"), NumberValue)) { Shot.Move.PanDegrees = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("tiltDegrees"), TEXT("tilt_degrees"), NumberValue)) { Shot.Move.TiltDegrees = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("forwardTravel"), TEXT("forward_travel"), NumberValue)) { Shot.Move.ForwardTravel = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("lateralTravel"), TEXT("lateral_travel"), NumberValue)) { Shot.Move.LateralTravel = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("verticalTravel"), TEXT("vertical_travel"), NumberValue)) { Shot.Move.VerticalTravel = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("handheldLocationAmplitude"), TEXT("handheld_location_amplitude"), NumberValue)) { Shot.Move.HandheldLocationAmplitude = static_cast<float>(NumberValue); }
			if (TryGetNumber(Move, TEXT("handheldRotationAmplitude"), TEXT("handheld_rotation_amplitude"), NumberValue)) { Shot.Move.HandheldRotationAmplitude = static_cast<float>(NumberValue); }
		}

		if (TSharedPtr<FJsonObject> Composition = GetObjectField(ShotObject, TEXT("composition")))
		{
			if (!ApplyEnumField(Composition, TEXT("rule"), TEXT("compositionRule"), Shot.Composition.Rule, OutError))
			{
				return false;
			}
			if (TryGetBool(Composition, TEXT("lookAtTarget"), TEXT("look_at_target"), BoolValue)) { Shot.Composition.bLookAtTarget = BoolValue; }
			if (TryGetNumber(Composition, TEXT("yawDegrees"), TEXT("yaw_degrees"), NumberValue)) { Shot.Composition.YawDegrees = static_cast<float>(NumberValue); }
			if (TryGetNumber(Composition, TEXT("pitchDegrees"), TEXT("pitch_degrees"), NumberValue)) { Shot.Composition.PitchDegrees = static_cast<float>(NumberValue); }
			if (TryGetNumber(Composition, TEXT("heightOffset"), TEXT("height_offset"), NumberValue)) { Shot.Composition.HeightOffset = static_cast<float>(NumberValue); }
			if (TryGetNumber(Composition, TEXT("sideOffset"), TEXT("side_offset"), NumberValue)) { Shot.Composition.SideOffset = static_cast<float>(NumberValue); }
			ReadVector(Composition, TEXT("targetOffset"), TEXT("target_offset"), Shot.Composition.TargetOffset);
			ReadVector2D(Composition, TEXT("screenOffset"), TEXT("screen_offset"), Shot.Composition.ScreenOffset);
		}

		if (TSharedPtr<FJsonObject> Lens = GetObjectField(ShotObject, TEXT("lens")))
		{
			if (TryGetNumber(Lens, TEXT("focalLength"), TEXT("focal_length"), NumberValue)) { Shot.Lens.FocalLength = FMath::Max(1.0f, static_cast<float>(NumberValue)); }
			if (TryGetNumber(Lens, TEXT("aperture"), TEXT("fstop"), NumberValue)) { Shot.Lens.Aperture = FMath::Max(0.1f, static_cast<float>(NumberValue)); }
			if (TryGetNumber(Lens, TEXT("focusDistance"), TEXT("focus_distance"), NumberValue)) { Shot.Lens.FocusDistance = FMath::Max(0.0f, static_cast<float>(NumberValue)); }
			if (TryGetBool(Lens, TEXT("autoFocusOnSubject"), TEXT("auto_focus_on_subject"), BoolValue)) { Shot.Lens.bAutoFocusOnSubject = BoolValue; }
		}

		return true;
	}

	TSharedRef<FJsonObject> ShotToJson(const FAIDirectorShot& Shot)
	{
		TSharedRef<FJsonObject> ShotObject = MakeShared<FJsonObject>();
		ShotObject->SetStringField(TEXT("shotName"), Shot.ShotName.ToString());
		ShotObject->SetBoolField(TEXT("enabled"), Shot.bEnabled);
		ShotObject->SetNumberField(TEXT("durationSeconds"), Shot.DurationSeconds);
		ShotObject->SetStringField(TEXT("notes"), Shot.Notes);

		TSharedRef<FJsonObject> Subject = MakeShared<FJsonObject>();
		Subject->SetStringField(TEXT("mode"), EnumToString(Shot.Subject.Mode));
		Subject->SetStringField(TEXT("actor"), Shot.Subject.Actor.ToSoftObjectPath().ToString());
		Subject->SetStringField(TEXT("actorTag"), Shot.Subject.ActorTag.ToString());
		Subject->SetObjectField(TEXT("worldLocation"), VectorToJson(Shot.Subject.WorldLocation));
		ShotObject->SetObjectField(TEXT("subject"), Subject);

		TSharedRef<FJsonObject> Move = MakeShared<FJsonObject>();
		Move->SetStringField(TEXT("moveType"), EnumToString(Shot.Move.MoveType));
		Move->SetStringField(TEXT("easing"), EnumToString(Shot.Move.Easing));
		Move->SetNumberField(TEXT("startDistance"), Shot.Move.StartDistance);
		Move->SetNumberField(TEXT("endDistance"), Shot.Move.EndDistance);
		Move->SetNumberField(TEXT("orbitDegrees"), Shot.Move.OrbitDegrees);
		Move->SetNumberField(TEXT("panDegrees"), Shot.Move.PanDegrees);
		Move->SetNumberField(TEXT("tiltDegrees"), Shot.Move.TiltDegrees);
		Move->SetNumberField(TEXT("forwardTravel"), Shot.Move.ForwardTravel);
		Move->SetNumberField(TEXT("lateralTravel"), Shot.Move.LateralTravel);
		Move->SetNumberField(TEXT("verticalTravel"), Shot.Move.VerticalTravel);
		Move->SetNumberField(TEXT("handheldLocationAmplitude"), Shot.Move.HandheldLocationAmplitude);
		Move->SetNumberField(TEXT("handheldRotationAmplitude"), Shot.Move.HandheldRotationAmplitude);
		ShotObject->SetObjectField(TEXT("move"), Move);

		TSharedRef<FJsonObject> Composition = MakeShared<FJsonObject>();
		Composition->SetStringField(TEXT("rule"), EnumToString(Shot.Composition.Rule));
		Composition->SetBoolField(TEXT("lookAtTarget"), Shot.Composition.bLookAtTarget);
		Composition->SetNumberField(TEXT("yawDegrees"), Shot.Composition.YawDegrees);
		Composition->SetNumberField(TEXT("pitchDegrees"), Shot.Composition.PitchDegrees);
		Composition->SetNumberField(TEXT("heightOffset"), Shot.Composition.HeightOffset);
		Composition->SetNumberField(TEXT("sideOffset"), Shot.Composition.SideOffset);
		Composition->SetObjectField(TEXT("targetOffset"), VectorToJson(Shot.Composition.TargetOffset));
		Composition->SetObjectField(TEXT("screenOffset"), Vector2DToJson(Shot.Composition.ScreenOffset));
		ShotObject->SetObjectField(TEXT("composition"), Composition);

		TSharedRef<FJsonObject> Lens = MakeShared<FJsonObject>();
		Lens->SetNumberField(TEXT("focalLength"), Shot.Lens.FocalLength);
		Lens->SetNumberField(TEXT("aperture"), Shot.Lens.Aperture);
		Lens->SetNumberField(TEXT("focusDistance"), Shot.Lens.FocusDistance);
		Lens->SetBoolField(TEXT("autoFocusOnSubject"), Shot.Lens.bAutoFocusOnSubject);
		ShotObject->SetObjectField(TEXT("lens"), Lens);

		return ShotObject;
	}

	TSharedRef<FJsonObject> SequenceToJson(UAIDirectorSequenceAsset* Sequence)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Sequence)
		{
			return Result;
		}

		Result->SetStringField(TEXT("assetPath"), Sequence->GetOutermost()->GetName());
		Result->SetStringField(TEXT("objectPath"), Sequence->GetPathName());
		Result->SetNumberField(TEXT("assetVersion"), Sequence->AssetVersion);
		Result->SetNumberField(TEXT("outputFrameRate"), Sequence->OutputFrameRate);
		Result->SetNumberField(TEXT("totalDurationSeconds"), Sequence->GetTotalDurationSeconds());
		Result->SetNumberField(TEXT("enabledShotCount"), Sequence->GetEnabledShotCount());

		TArray<TSharedPtr<FJsonValue>> Shots;
		Shots.Reserve(Sequence->Shots.Num());
		for (const FAIDirectorShot& Shot : Sequence->Shots)
		{
			Shots.Add(MakeShared<FJsonValueObject>(ShotToJson(Shot)));
		}
		Result->SetArrayField(TEXT("shots"), Shots);
		return Result;
	}

	TSharedPtr<FJsonObject> GetShotPayload(const TSharedPtr<FJsonObject>& Args)
	{
		TSharedPtr<FJsonObject> Shot = GetObjectField(Args, TEXT("shot"));
		return Shot.IsValid() ? Shot : Args;
	}

	int32 FindShotIndex(UAIDirectorSequenceAsset* Sequence, const TSharedPtr<FJsonObject>& Args, FString& OutError)
	{
		if (!Sequence)
		{
			OutError = TEXT("Missing sequence asset.");
			return INDEX_NONE;
		}

		int32 ShotIndex = INDEX_NONE;
		if (TryGetInt(Args, TEXT("shotIndex"), TEXT("index"), ShotIndex))
		{
			if (!Sequence->Shots.IsValidIndex(ShotIndex))
			{
				OutError = FString::Printf(TEXT("shotIndex %d is out of range."), ShotIndex);
				return INDEX_NONE;
			}
			return ShotIndex;
		}

		FString ShotName;
		if (TryGetString(Args, TEXT("shotName"), TEXT("name"), ShotName))
		{
			for (int32 Index = 0; Index < Sequence->Shots.Num(); ++Index)
			{
				if (Sequence->Shots[Index].ShotName.ToString().Equals(ShotName, ESearchCase::IgnoreCase))
				{
					return Index;
				}
			}
			OutError = FString::Printf(TEXT("Shot '%s' not found."), *ShotName);
			return INDEX_NONE;
		}

		OutError = TEXT("Missing 'shotIndex' or 'shotName'.");
		return INDEX_NONE;
	}

	FString CreateAsset(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString AssetName;
		FString Error;
		if (!ResolveAssetPath(Args, PackageName, AssetName, Error))
		{
			return ErrorJson(Error);
		}

		int32 OutputFrameRate = 24;
		TryGetInt(Args, TEXT("outputFrameRate"), TEXT("output_frame_rate"), OutputFrameRate);

		UAIDirectorSequenceAsset* Sequence = CreateDirectorAssetAtPath(PackageName, AssetName, OutputFrameRate, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		TSharedRef<FJsonObject> Result = SequenceToJson(Sequence);
		Result->SetBoolField(TEXT("created"), true);
		return SuccessJson(Result);
	}

	FString GetAsset(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString Error;
		UAIDirectorSequenceAsset* Sequence = LoadDirectorAsset(Args, PackageName, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		return SuccessJson(SequenceToJson(Sequence));
	}

	FString ApplySequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString AssetName;
		FString Error;
		if (!ResolveAssetPath(Args, PackageName, AssetName, Error))
		{
			return ErrorJson(Error);
		}

		UAIDirectorSequenceAsset* Sequence = Cast<UAIDirectorSequenceAsset>(UEditorAssetLibrary::LoadAsset(PackageName));
		if (!Sequence)
		{
			bool bCreateIfMissing = false;
			TryGetBool(Args, TEXT("createIfMissing"), TEXT("create_if_missing"), bCreateIfMissing);
			if (!bCreateIfMissing)
			{
				return ErrorJson(FString::Printf(TEXT("AI Director asset not found: %s"), *PackageName));
			}

			int32 OutputFrameRate = 24;
			TryGetInt(Args, TEXT("outputFrameRate"), TEXT("output_frame_rate"), OutputFrameRate);
			Sequence = CreateDirectorAssetAtPath(PackageName, AssetName, OutputFrameRate, Error);
			if (!Sequence)
			{
				return ErrorJson(Error);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
		if (!TryGetArray(Args, TEXT("shots"), Shots) || !Shots)
		{
			return ErrorJson(TEXT("Missing 'shots' array."));
		}

		int32 OutputFrameRate = Sequence->OutputFrameRate;
		if (TryGetInt(Args, TEXT("outputFrameRate"), TEXT("output_frame_rate"), OutputFrameRate))
		{
			Sequence->OutputFrameRate = FMath::Clamp(OutputFrameRate, 1, 240);
		}

		Sequence->Modify();
		Sequence->Shots.Reset();

		for (int32 Index = 0; Index < Shots->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* ShotObject = nullptr;
			if (!(*Shots)[Index].IsValid() || !(*Shots)[Index]->TryGetObject(ShotObject) || !ShotObject || !ShotObject->IsValid())
			{
				return ErrorJson(FString::Printf(TEXT("Shot at index %d must be a JSON object."), Index));
			}

			FAIDirectorShot Shot;
			Shot.ShotName = FName(*FString::Printf(TEXT("Shot_%02d"), Index + 1));
			if (!ApplyShotJson(*ShotObject, Shot, Error))
			{
				return ErrorJson(Error);
			}
			Sequence->Shots.Add(Shot);
		}

		SaveDirectorAsset(Sequence);
		return SuccessJson(SequenceToJson(Sequence));
	}

	FString AddShot(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString Error;
		UAIDirectorSequenceAsset* Sequence = LoadDirectorAsset(Args, PackageName, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		FAIDirectorShot Shot;
		Shot.ShotName = FName(*FString::Printf(TEXT("Shot_%02d"), Sequence->Shots.Num() + 1));
		if (!ApplyShotJson(GetShotPayload(Args), Shot, Error))
		{
			return ErrorJson(Error);
		}

		Sequence->Modify();
		int32 InsertIndex = Sequence->Shots.Num();
		TryGetInt(Args, TEXT("insertIndex"), TEXT("index"), InsertIndex);
		InsertIndex = FMath::Clamp(InsertIndex, 0, Sequence->Shots.Num());
		Sequence->Shots.Insert(Shot, InsertIndex);
		SaveDirectorAsset(Sequence);

		TSharedRef<FJsonObject> Result = SequenceToJson(Sequence);
		Result->SetNumberField(TEXT("shotIndex"), InsertIndex);
		return SuccessJson(Result);
	}

	FString UpdateShot(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString Error;
		UAIDirectorSequenceAsset* Sequence = LoadDirectorAsset(Args, PackageName, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		const int32 ShotIndex = FindShotIndex(Sequence, Args, Error);
		if (ShotIndex == INDEX_NONE)
		{
			return ErrorJson(Error);
		}

		Sequence->Modify();
		if (!ApplyShotJson(GetShotPayload(Args), Sequence->Shots[ShotIndex], Error))
		{
			return ErrorJson(Error);
		}
		SaveDirectorAsset(Sequence);

		TSharedRef<FJsonObject> Result = SequenceToJson(Sequence);
		Result->SetNumberField(TEXT("shotIndex"), ShotIndex);
		return SuccessJson(Result);
	}

	FString GenerateLevelSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString Error;
		UAIDirectorSequenceAsset* Sequence = LoadDirectorAsset(Args, PackageName, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FString LevelSequenceAssetPath;
		if (!TryGetString(Args, TEXT("levelSequenceAssetPath"), TEXT("sequencePath"), LevelSequenceAssetPath))
		{
			LevelSequenceAssetPath = FString::Printf(TEXT("/Game/Sequences/LS_%s_Prompt"), *Sequence->GetName());
		}

		int32 SamplesPerSecond = Sequence->OutputFrameRate;
		TryGetInt(Args, TEXT("samplesPerSecond"), TEXT("samples_per_second"), SamplesPerSecond);

		bool bSpawnCamerasInLevel = true;
		TryGetBool(Args, TEXT("spawnCamerasInLevel"), TEXT("spawn_cameras_in_level"), bSpawnCamerasInLevel);

		ULevelSequence* CreatedSequence = UAIDirectorSequencerLibrary::CreatePromptLevelSequence(
			World,
			Sequence,
			LevelSequenceAssetPath,
			SamplesPerSecond,
			bSpawnCamerasInLevel);
		if (!CreatedSequence)
		{
			return ErrorJson(TEXT("Failed to generate Level Sequence from AI Director asset."));
		}

		UEditorAssetLibrary::SaveLoadedAsset(CreatedSequence, false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("directorAssetPath"), Sequence->GetOutermost()->GetName());
		Result->SetStringField(TEXT("levelSequencePath"), CreatedSequence->GetOutermost()->GetName());
		Result->SetStringField(TEXT("levelSequenceObjectPath"), CreatedSequence->GetPathName());
		Result->SetNumberField(TEXT("samplesPerSecond"), SamplesPerSecond);
		Result->SetBoolField(TEXT("spawnedCamerasInLevel"), bSpawnCamerasInLevel);
		return SuccessJson(Result);
	}

	FString ExportPromptJson(const TSharedPtr<FJsonObject>& Args)
	{
		FString PackageName;
		FString Error;
		UAIDirectorSequenceAsset* Sequence = LoadDirectorAsset(Args, PackageName, Error);
		if (!Sequence)
		{
			return ErrorJson(Error);
		}

		FString AbsoluteFilePath;
		if (!TryGetString(Args, TEXT("absoluteFilePath"), TEXT("filePath"), AbsoluteFilePath))
		{
			const FString ExportDir = FPaths::ProjectSavedDir() / TEXT("AIDirector");
			AbsoluteFilePath = ExportDir / (Sequence->GetName() + TEXT(".json"));
		}

		FString Directory = FPaths::GetPath(AbsoluteFilePath);
		if (!Directory.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
		}

		bool bPrettyPrint = true;
		TryGetBool(Args, TEXT("prettyPrint"), TEXT("pretty_print"), bPrettyPrint);

		if (!UAIDirectorJsonExportLibrary::ExportDirectorSequenceToJson(Sequence, AbsoluteFilePath, bPrettyPrint))
		{
			return ErrorJson(FString::Printf(TEXT("Failed to export AI Director JSON: %s"), *AbsoluteFilePath));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("directorAssetPath"), Sequence->GetOutermost()->GetName());
		Result->SetStringField(TEXT("filePath"), AbsoluteFilePath);
		Result->SetBoolField(TEXT("prettyPrint"), bPrettyPrint);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"ai_director_create_asset","description":"Create a UAIDirectorSequenceAsset. Provide assetPath, or packagePath + name. Optional outputFrameRate.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Package path such as /Game/AIDirector/DA_ShotPlan."},"packagePath":{"type":"string"},"name":{"type":"string"},"outputFrameRate":{"type":"number"}}},"annotations":{"title":"AI Director Create Asset","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"ai_director_get_asset","description":"Read a UAIDirectorSequenceAsset and return its shot list as MCP JSON.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"path":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"AI Director Get Asset","readOnlyHint":true,"openWorldHint":false}},
{"name":"ai_director_apply_sequence","description":"Replace the full shot list of a UAIDirectorSequenceAsset. Set createIfMissing=true to create the asset if needed.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"createIfMissing":{"type":"boolean"},"outputFrameRate":{"type":"number"},"shots":{"type":"array","description":"Array of shot objects."}},"required":["assetPath","shots"]},"annotations":{"title":"AI Director Apply Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"ai_director_add_shot","description":"Append or insert one shot into a UAIDirectorSequenceAsset. Provide either a shot object or shot fields at the top level.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"insertIndex":{"type":"number"},"shot":{"type":"object"}},"required":["assetPath"]},"annotations":{"title":"AI Director Add Shot","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"ai_director_update_shot","description":"Patch one shot by zero-based shotIndex or shotName. Provide either a shot object or shot fields at the top level.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"shotIndex":{"type":"number"},"shotName":{"type":"string"},"shot":{"type":"object"}},"required":["assetPath"]},"annotations":{"title":"AI Director Update Shot","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"ai_director_generate_level_sequence","description":"Generate a Level Sequence from a UAIDirectorSequenceAsset using the AI Director camera solver.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"levelSequenceAssetPath":{"type":"string"},"samplesPerSecond":{"type":"number"},"spawnCamerasInLevel":{"type":"boolean"}},"required":["assetPath"]},"annotations":{"title":"AI Director Generate Level Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"ai_director_export_prompt_json","description":"Export the UAIDirectorSequenceAsset as prompt-video JSON for external AI rendering.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"absoluteFilePath":{"type":"string"},"prettyPrint":{"type":"boolean"}},"required":["assetPath"]},"annotations":{"title":"AI Director Export Prompt JSON","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("ai_director_create_asset")) { OutResult = CreateAsset(Args); return true; }
	if (ToolName == TEXT("ai_director_get_asset")) { OutResult = GetAsset(Args); return true; }
	if (ToolName == TEXT("ai_director_apply_sequence")) { OutResult = ApplySequence(Args); return true; }
	if (ToolName == TEXT("ai_director_add_shot")) { OutResult = AddShot(Args); return true; }
	if (ToolName == TEXT("ai_director_update_shot")) { OutResult = UpdateShot(Args); return true; }
	if (ToolName == TEXT("ai_director_generate_level_sequence")) { OutResult = GenerateLevelSequence(Args); return true; }
	if (ToolName == TEXT("ai_director_export_prompt_json")) { OutResult = ExportPromptJson(Args); return true; }
	return false;
}
}
}
