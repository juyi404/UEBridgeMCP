#include "WorldDataMCPSceneTools.h"

#include "WorldDataMCPCommon.h"

#include "Components/AudioComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "InstancedFoliageActor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"
#include "UObject/UnrealType.h"

namespace WorldDataMCP
{
namespace SceneTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return GWorld;
	}

	AActor* FindActor(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		AActor* CaseInsensitive = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (Actor->GetActorLabel() == NameOrLabel || Actor->GetName() == NameOrLabel)
			{
				return Actor;
			}
			if (!CaseInsensitive
				&& (Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase)
					|| Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)))
			{
				CaseInsensitive = Actor;
			}
		}
		return CaseInsensitive;
	}

	// Parse {x,y,z} object or [x,y,z] array into a vector. Returns false if absent.
	bool TryGetVector(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(Field, Arr) && Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& A = *Arr;
			Out = FVector(
				A.Num() > 0 ? A[0]->AsNumber() : 0.0,
				A.Num() > 1 ? A[1]->AsNumber() : 0.0,
				A.Num() > 2 ? A[2]->AsNumber() : 0.0);
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Args->TryGetObjectField(Field, Obj) && Obj && Obj->IsValid())
		{
			double X = 0, Y = 0, Z = 0;
			(*Obj)->TryGetNumberField(TEXT("x"), X);
			(*Obj)->TryGetNumberField(TEXT("y"), Y);
			(*Obj)->TryGetNumberField(TEXT("z"), Z);
			Out = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	// Parse [r,g,b] or [r,g,b,a] color array. Returns false if absent.
	bool TryGetColor(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FLinearColor& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(Field, Arr) && Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>& A = *Arr;
			Out.R = A.Num() > 0 ? static_cast<float>(A[0]->AsNumber()) : 0.0f;
			Out.G = A.Num() > 1 ? static_cast<float>(A[1]->AsNumber()) : 0.0f;
			Out.B = A.Num() > 2 ? static_cast<float>(A[2]->AsNumber()) : 0.0f;
			Out.A = A.Num() > 3 ? static_cast<float>(A[3]->AsNumber()) : 1.0f;
			return true;
		}
		return false;
	}

	bool TryParseMobility(const FString& Name, EComponentMobility::Type& Out)
	{
		if (Name.Equals(TEXT("Static"), ESearchCase::IgnoreCase)) { Out = EComponentMobility::Static; return true; }
		if (Name.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase)) { Out = EComponentMobility::Stationary; return true; }
		if (Name.Equals(TEXT("Movable"), ESearchCase::IgnoreCase)) { Out = EComponentMobility::Movable; return true; }
		return false;
	}

	// Apply a flat {name: value} map of properties onto an object via reflection. Used for
	// struct-heavy actors (sky atmosphere) where dedicated setters vary across engine versions.
	int32 ApplyReflectedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutApplied, TArray<FString>& OutFailed)
	{
		if (!Target || !Props.IsValid())
		{
			return 0;
		}
		int32 Count = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*Pair.Key));
			if (!Property)
			{
				OutFailed.Add(Pair.Key);
				continue;
			}
			FString TextValue;
			if (Pair.Value->Type == EJson::Number)
			{
				TextValue = FString::SanitizeFloat(Pair.Value->AsNumber());
			}
			else if (Pair.Value->Type == EJson::Boolean)
			{
				TextValue = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				TextValue = Pair.Value->AsString();
			}
			void* Addr = Property->ContainerPtrToValuePtr<void>(Target);
			const TCHAR* Result = Property->ImportText_Direct(*TextValue, Addr, Target, PPF_None);
			if (Result != nullptr)
			{
				OutApplied.Add(Pair.Key);
				++Count;
			}
			else
			{
				OutFailed.Add(Pair.Key);
			}
		}
		return Count;
	}

	// ---- Environment & lighting ---------------------------------------------------------

	FString SetLightProperties(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name' (light actor)."));
		}
		UWorld* World = GetEditorWorld();
		AActor* Actor = FindActor(World, Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>();
		if (!Light)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' has no light component."), *Name));
		}
		Actor->Modify();

		double Intensity = 0.0;
		if (Args->TryGetNumberField(TEXT("intensity"), Intensity))
		{
			Light->SetIntensity(static_cast<float>(Intensity));
		}
		FLinearColor Color;
		if (TryGetColor(Args, TEXT("color"), Color))
		{
			Light->SetLightColor(Color);
		}
		bool bCastShadows = false;
		if (Args->TryGetBoolField(TEXT("castShadows"), bCastShadows))
		{
			Light->SetCastShadows(bCastShadows);
		}
		double AttenuationRadius = 0.0;
		if (Args->TryGetNumberField(TEXT("attenuationRadius"), AttenuationRadius))
		{
			if (UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
			{
				Point->SetAttenuationRadius(static_cast<float>(AttenuationRadius));
			}
		}
		FString MobilityStr;
		EComponentMobility::Type Mobility;
		if (Args->TryGetStringField(TEXT("mobility"), MobilityStr) && TryParseMobility(MobilityStr, Mobility))
		{
			Light->SetMobility(Mobility);
		}
		Light->MarkRenderStateDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("lightComponent"), Light->GetClass()->GetName());
		Result->SetNumberField(TEXT("intensity"), Light->Intensity);
		return SuccessJson(Result);
	}

	FString SetFog(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		AExponentialHeightFog* Fog = nullptr;
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
		{
			Fog = *It;
			break;
		}
		if (!Fog)
		{
			return ErrorJson(TEXT("No ExponentialHeightFog actor in the level. Spawn one first."));
		}
		UExponentialHeightFogComponent* FC = Fog->GetComponent();
		if (!FC)
		{
			return ErrorJson(TEXT("Fog actor has no component."));
		}
		Fog->Modify();

		double Density = 0.0;
		if (Args->TryGetNumberField(TEXT("density"), Density)) { FC->FogDensity = static_cast<float>(Density); }
		double HeightFalloff = 0.0;
		if (Args->TryGetNumberField(TEXT("heightFalloff"), HeightFalloff)) { FC->FogHeightFalloff = static_cast<float>(HeightFalloff); }
		double StartDistance = 0.0;
		if (Args->TryGetNumberField(TEXT("startDistance"), StartDistance)) { FC->StartDistance = static_cast<float>(StartDistance); }
		FLinearColor Color;
		if (TryGetColor(Args, TEXT("color"), Color)) { FC->FogInscatteringLuminance = Color; }
		FC->MarkRenderStateDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("fogActor"), Fog->GetActorLabel());
		Result->SetNumberField(TEXT("density"), FC->FogDensity);
		Result->SetNumberField(TEXT("heightFalloff"), FC->FogHeightFalloff);
		return SuccessJson(Result);
	}

	FString SetSkyAtmosphere(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		ASkyAtmosphere* Sky = nullptr;
		for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
		{
			Sky = *It;
			break;
		}
		if (!Sky)
		{
			return ErrorJson(TEXT("No SkyAtmosphere actor in the level. Spawn one first."));
		}
		USkyAtmosphereComponent* Comp = Sky->GetComponent();
		if (!Comp)
		{
			return ErrorJson(TEXT("SkyAtmosphere actor has no component."));
		}
		Sky->Modify();

		// Properties are applied by reflection (e.g. RayleighScatteringScale, MieScatteringScale,
		// MultiScatteringFactor, AtmosphereHeight). Pass a 'properties' object of name->value.
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		TArray<FString> Applied, Failed;
		if (Args->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
		{
			ApplyReflectedProperties(Comp, *PropsPtr, Applied, Failed);
		}
		Comp->MarkRenderStateDirty();

		TArray<TSharedPtr<FJsonValue>> AppliedJson, FailedJson;
		for (const FString& A : Applied) { AppliedJson.Add(MakeShared<FJsonValueString>(A)); }
		for (const FString& F : Failed) { FailedJson.Add(MakeShared<FJsonValueString>(F)); }

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("skyActor"), Sky->GetActorLabel());
		Result->SetArrayField(TEXT("applied"), AppliedJson);
		Result->SetArrayField(TEXT("failed"), FailedJson);
		return SuccessJson(Result);
	}

	FString SetPostProcess(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		APostProcessVolume* Volume = nullptr;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			if (Name.IsEmpty() || (*It)->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
			{
				Volume = *It;
				break;
			}
		}
		if (!Volume)
		{
			return ErrorJson(TEXT("No matching PostProcessVolume in the level."));
		}
		Volume->Modify();
		FPostProcessSettings& S = Volume->Settings;

		// Each setting needs its bOverride flag set to take effect.
		double Value = 0.0;
		if (Args->TryGetNumberField(TEXT("exposureBias"), Value)) { S.bOverride_AutoExposureBias = true; S.AutoExposureBias = static_cast<float>(Value); }
		if (Args->TryGetNumberField(TEXT("bloomIntensity"), Value)) { S.bOverride_BloomIntensity = true; S.BloomIntensity = static_cast<float>(Value); }
		if (Args->TryGetNumberField(TEXT("vignetteIntensity"), Value)) { S.bOverride_VignetteIntensity = true; S.VignetteIntensity = static_cast<float>(Value); }
		FLinearColor Color;
		if (TryGetColor(Args, TEXT("colorSaturation"), Color)) { S.bOverride_ColorSaturation = true; S.ColorSaturation = FVector4(Color.R, Color.G, Color.B, Color.A); }

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("volume"), Volume->GetActorLabel());
		return SuccessJson(Result);
	}

	// ---- Level info & validation --------------------------------------------------------

	FString GetLevelInfo(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		int32 ActorCount = 0;
		int32 LightCount = 0;
		TMap<FString, int32> CountsByClass;
		FBox Bounds(ForceInit);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			++ActorCount;
			CountsByClass.FindOrAdd(Actor->GetClass()->GetName())++;
			if (Actor->FindComponentByClass<ULightComponent>())
			{
				++LightCount;
			}
			Bounds += Actor->GetActorLocation();
		}

		CountsByClass.ValueSort([](const int32& A, const int32& B) { return A > B; });
		TArray<TSharedPtr<FJsonValue>> TopClasses;
		for (const TPair<FString, int32>& Pair : CountsByClass)
		{
			if (TopClasses.Num() >= 15)
			{
				break;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), Pair.Key);
			Entry->SetNumberField(TEXT("count"), Pair.Value);
			TopClasses.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("level"), World->GetMapName());
		Result->SetNumberField(TEXT("actorCount"), ActorCount);
		Result->SetNumberField(TEXT("lightCount"), LightCount);
		Result->SetNumberField(TEXT("uniqueClasses"), CountsByClass.Num());
		Result->SetArrayField(TEXT("topClasses"), TopClasses);
		if (Bounds.IsValid)
		{
			const FVector Size = Bounds.GetSize();
			TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			BoundsObj->SetNumberField(TEXT("sizeX"), Size.X);
			BoundsObj->SetNumberField(TEXT("sizeY"), Size.Y);
			BoundsObj->SetNumberField(TEXT("sizeZ"), Size.Z);
			Result->SetObjectField(TEXT("actorBounds"), BoundsObj);
		}
		return SuccessJson(Result);
	}

	FString GetWorldSettings(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		AWorldSettings* WS = World ? World->GetWorldSettings() : nullptr;
		if (!WS)
		{
			return ErrorJson(TEXT("World settings not available."));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("worldGravityZ"), WS->GetGravityZ());
		Result->SetBoolField(TEXT("globalGravitySet"), WS->bGlobalGravitySet);
		Result->SetNumberField(TEXT("killZ"), WS->KillZ);
		Result->SetNumberField(TEXT("worldToMeters"), WS->WorldToMeters);
		Result->SetStringField(TEXT("defaultGameMode"),
			WS->DefaultGameMode ? WS->DefaultGameMode->GetName() : FString(TEXT("(none)")));
		return SuccessJson(Result);
	}

	// Tee GLog so the diagnostic output of a console-driven command is captured.
	class FCaptureOutputDevice : public FOutputDevice
	{
	public:
		FString Captured;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override { Captured += V; Captured += TEXT("\n"); }
	};

	FString RunCapturedCommand(const FString& Command)
	{
		UWorld* World = GetEditorWorld();
		FCaptureOutputDevice Capture;
		if (GLog) { GLog->AddOutputDevice(&Capture); }
		const bool bHandled = GEngine ? GEngine->Exec(World, *Command, Capture) : false;
		if (GLog) { GLog->Flush(); GLog->RemoveOutputDevice(&Capture); }

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("command"), Command);
		Result->SetBoolField(TEXT("handled"), bHandled);
		Result->SetStringField(TEXT("output"), Capture.Captured);
		return SuccessJson(Result);
	}

	FString GetMapCheckErrors(const TSharedPtr<FJsonObject>& Args)
	{
		// Map Check primarily routes to the Message Log; captured GLog output may be partial.
		return RunCapturedCommand(TEXT("MAP CHECK"));
	}

	FString ValidateAssets(const TSharedPtr<FJsonObject>& Args)
	{
		FString Path = TEXT("/Game/");
		Args->TryGetStringField(TEXT("path"), Path);
		return RunCapturedCommand(FString::Printf(TEXT("DataValidation.ValidateAssets %s"), *Path));
	}

	FString GetNavigationInfo(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("navigationSystemPresent"), NavSys != nullptr);
		if (NavSys)
		{
			Result->SetNumberField(TEXT("navDataCount"), NavSys->NavDataSet.Num());
			Result->SetBoolField(TEXT("isNavigationBuilt"), NavSys->IsNavigationBuilt(World ? World->GetWorldSettings() : nullptr));
		}
		return SuccessJson(Result);
	}

	// ---- Splines & foliage --------------------------------------------------------------

	FString CreateSplineActor(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		FVector Location = FVector::ZeroVector;
		TryGetVector(Args, TEXT("location"), Location);

		FActorSpawnParameters SpawnParams;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			return ErrorJson(TEXT("Failed to spawn spline actor."));
		}
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		USplineComponent* Spline = NewObject<USplineComponent>(Actor, TEXT("Spline"));
		Spline->SetupAttachment(Root);
		Spline->RegisterComponent();
		Actor->AddInstanceComponent(Spline);

		FString Label;
		if (Args->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Actor->SetActorLabel(Label);
		}

		// Optionally seed points.
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (Args->TryGetArrayField(TEXT("points"), Points) && Points)
		{
			Spline->ClearSplinePoints(false);
			for (const TSharedPtr<FJsonValue>& PointValue : *Points)
			{
				const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
				if (PointValue->TryGetArray(P) && P && P->Num() >= 3)
				{
					const FVector Pt((*P)[0]->AsNumber(), (*P)[1]->AsNumber(), (*P)[2]->AsNumber());
					Spline->AddSplinePoint(Pt, ESplineCoordinateSpace::World, false);
				}
			}
			Spline->UpdateSpline();
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetNumberField(TEXT("splinePoints"), Spline->GetNumberOfSplinePoints());
		return SuccessJson(Result);
	}

	USplineComponent* ResolveSpline(UWorld* World, const FString& Name)
	{
		AActor* Actor = FindActor(World, Name);
		return Actor ? Actor->FindComponentByClass<USplineComponent>() : nullptr;
	}

	FString SetSplinePoints(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		USplineComponent* Spline = ResolveSpline(GetEditorWorld(), Name);
		if (!Spline)
		{
			return ErrorJson(FString::Printf(TEXT("No spline component on actor '%s'."), *Name));
		}
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Args->TryGetArrayField(TEXT("points"), Points) || !Points)
		{
			return ErrorJson(TEXT("Missing 'points' array of [x,y,z] entries."));
		}
		Spline->Modify();
		Spline->ClearSplinePoints(false);
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (PointValue->TryGetArray(P) && P && P->Num() >= 3)
			{
				const FVector Pt((*P)[0]->AsNumber(), (*P)[1]->AsNumber(), (*P)[2]->AsNumber());
				Spline->AddSplinePoint(Pt, ESplineCoordinateSpace::World, false);
			}
		}
		bool bClosed = false;
		if (Args->TryGetBoolField(TEXT("closedLoop"), bClosed))
		{
			Spline->SetClosedLoop(bClosed);
		}
		Spline->UpdateSpline();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetNumberField(TEXT("splinePoints"), Spline->GetNumberOfSplinePoints());
		Result->SetNumberField(TEXT("length"), Spline->GetSplineLength());
		return SuccessJson(Result);
	}

	FString GetSplineInfo(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		USplineComponent* Spline = ResolveSpline(GetEditorWorld(), Name);
		if (!Spline)
		{
			return ErrorJson(FString::Printf(TEXT("No spline component on actor '%s'."), *Name));
		}
		const int32 Num = Spline->GetNumberOfSplinePoints();
		TArray<TSharedPtr<FJsonValue>> PointsJson;
		for (int32 i = 0; i < Num; ++i)
		{
			const FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), i);
			Entry->SetNumberField(TEXT("x"), Loc.X);
			Entry->SetNumberField(TEXT("y"), Loc.Y);
			Entry->SetNumberField(TEXT("z"), Loc.Z);
			PointsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetNumberField(TEXT("numPoints"), Num);
		Result->SetBoolField(TEXT("closedLoop"), Spline->IsClosedLoop());
		Result->SetNumberField(TEXT("length"), Spline->GetSplineLength());
		Result->SetArrayField(TEXT("points"), PointsJson);
		return SuccessJson(Result);
	}

	FString GetFoliageStats(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		int64 TotalInstances = 0;
		TArray<TSharedPtr<FJsonValue>> Types;
		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			AInstancedFoliageActor* FoliageActor = *It;
			if (!IsValid(FoliageActor))
			{
				continue;
			}
			for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : FoliageActor->GetFoliageInfos())
			{
				const UFoliageType* Type = Pair.Key;
				const FFoliageInfo& Info = *Pair.Value;
				int32 InstanceCount = 0;
				if (UHierarchicalInstancedStaticMeshComponent* HISM = Info.GetComponent())
				{
					InstanceCount = HISM->GetInstanceCount();
				}
				TotalInstances += InstanceCount;
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("foliageType"), Type ? Type->GetName() : FString(TEXT("(null)")));
				Entry->SetNumberField(TEXT("instances"), InstanceCount);
				Types.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("totalInstances"), static_cast<double>(TotalInstances));
		Result->SetNumberField(TEXT("typeCount"), Types.Num());
		Result->SetArrayField(TEXT("types"), Types);
		return SuccessJson(Result);
	}

	// ---- Physics, collision & audio -----------------------------------------------------

	FString SetPhysicsSimulation(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		bool bEnabled = false;
		Args->TryGetBoolField(TEXT("enabled"), bEnabled);
		AActor* Actor = FindActor(GetEditorWorld(), Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents<UPrimitiveComponent>(Primitives);
		int32 Affected = 0;
		Actor->Modify();
		for (UPrimitiveComponent* Prim : Primitives)
		{
			Prim->SetSimulatePhysics(bEnabled);
			++Affected;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetActorLabel());
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		Result->SetNumberField(TEXT("componentsAffected"), Affected);
		return SuccessJson(Result);
	}

	FString SetCollisionProfile(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		FString Profile;
		Args->TryGetStringField(TEXT("profile"), Profile);
		if (Name.IsEmpty() || Profile.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'name' and 'profile' are required (e.g. profile 'BlockAll')."));
		}
		AActor* Actor = FindActor(GetEditorWorld(), Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents<UPrimitiveComponent>(Primitives);
		int32 Affected = 0;
		Actor->Modify();
		for (UPrimitiveComponent* Prim : Primitives)
		{
			Prim->SetCollisionProfileName(FName(*Profile));
			++Affected;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("profile"), Profile);
		Result->SetNumberField(TEXT("componentsAffected"), Affected);
		return SuccessJson(Result);
	}

	FString SetCollisionEnabled(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		FString Mode;
		Args->TryGetStringField(TEXT("mode"), Mode);
		ECollisionEnabled::Type Enabled;
		if (Mode.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase)) { Enabled = ECollisionEnabled::NoCollision; }
		else if (Mode.Equals(TEXT("QueryOnly"), ESearchCase::IgnoreCase)) { Enabled = ECollisionEnabled::QueryOnly; }
		else if (Mode.Equals(TEXT("PhysicsOnly"), ESearchCase::IgnoreCase)) { Enabled = ECollisionEnabled::PhysicsOnly; }
		else if (Mode.Equals(TEXT("QueryAndPhysics"), ESearchCase::IgnoreCase)) { Enabled = ECollisionEnabled::QueryAndPhysics; }
		else { return ErrorJson(TEXT("'mode' must be NoCollision, QueryOnly, PhysicsOnly, or QueryAndPhysics.")); }

		AActor* Actor = FindActor(GetEditorWorld(), Name);
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *Name));
		}
		TArray<UPrimitiveComponent*> Primitives;
		Actor->GetComponents<UPrimitiveComponent>(Primitives);
		int32 Affected = 0;
		Actor->Modify();
		for (UPrimitiveComponent* Prim : Primitives)
		{
			Prim->SetCollisionEnabled(Enabled);
			++Affected;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("mode"), Mode);
		Result->SetNumberField(TEXT("componentsAffected"), Affected);
		return SuccessJson(Result);
	}

	USoundBase* LoadSound(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		FString Normalized = Path;
		Normalized.TrimStartAndEndInline();
		if (!Normalized.Contains(TEXT(".")))
		{
			Normalized = FString::Printf(TEXT("%s.%s"), *Normalized, *FPaths::GetBaseFilename(Normalized));
		}
		return Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, *Normalized));
	}

	FString PlaySoundAtLocation(const TSharedPtr<FJsonObject>& Args)
	{
		FString SoundPath;
		Args->TryGetStringField(TEXT("soundPath"), SoundPath);
		USoundBase* Sound = LoadSound(SoundPath);
		if (!Sound)
		{
			return ErrorJson(FString::Printf(TEXT("Sound '%s' not found."), *SoundPath));
		}
		UWorld* World = GetEditorWorld();
		FVector Location = FVector::ZeroVector;
		TryGetVector(Args, TEXT("location"), Location);
		double Volume = 1.0;
		Args->TryGetNumberField(TEXT("volume"), Volume);
		double Pitch = 1.0;
		Args->TryGetNumberField(TEXT("pitch"), Pitch);
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Location, static_cast<float>(Volume), static_cast<float>(Pitch));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("sound"), Sound->GetPathName());
		Result->SetStringField(TEXT("note"), TEXT("Previewed in the editor world; not a persistent placement."));
		return SuccessJson(Result);
	}

	FString SpawnAmbientSound(const TSharedPtr<FJsonObject>& Args)
	{
		FString SoundPath;
		Args->TryGetStringField(TEXT("soundPath"), SoundPath);
		USoundBase* Sound = LoadSound(SoundPath);
		if (!Sound)
		{
			return ErrorJson(FString::Printf(TEXT("Sound '%s' not found."), *SoundPath));
		}
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		FVector Location = FVector::ZeroVector;
		TryGetVector(Args, TEXT("location"), Location);
		AAmbientSound* Actor = World->SpawnActor<AAmbientSound>(AAmbientSound::StaticClass(), Location, FRotator::ZeroRotator);
		if (!Actor)
		{
			return ErrorJson(TEXT("Failed to spawn AmbientSound actor."));
		}
		if (UAudioComponent* AudioComp = Actor->GetAudioComponent())
		{
			AudioComp->SetSound(Sound);
			double Volume = 1.0;
			if (Args->TryGetNumberField(TEXT("volume"), Volume))
			{
				AudioComp->VolumeMultiplier = static_cast<float>(Volume);
			}
		}
		FString Label;
		if (Args->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Actor->SetActorLabel(Label);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Actor->GetName());
		Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("sound"), Sound->GetPathName());
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"set_light_properties","description":"Set properties on a light actor's light component: intensity, color [r,g,b], castShadows, attenuationRadius (point/spot), mobility (Static/Stationary/Movable).","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Light actor name or label."},"intensity":{"type":"number"},"color":{"type":"array","items":{"type":"number"}},"castShadows":{"type":"boolean"},"attenuationRadius":{"type":"number"},"mobility":{"type":"string"}},"required":["name"]},"annotations":{"title":"Set Light Properties","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_fog","description":"Set the level's ExponentialHeightFog: density, heightFalloff, startDistance, color [r,g,b].","inputSchema":{"type":"object","properties":{"density":{"type":"number"},"heightFalloff":{"type":"number"},"startDistance":{"type":"number"},"color":{"type":"array","items":{"type":"number"}}}},"annotations":{"title":"Set Fog","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_sky_atmosphere","description":"Set properties on the level's SkyAtmosphere component via reflection. Pass a 'properties' object of UPROPERTY name -> value (e.g. RayleighScatteringScale, MieScatteringScale, MultiScatteringFactor).","inputSchema":{"type":"object","properties":{"properties":{"type":"object"}},"required":["properties"]},"annotations":{"title":"Set Sky Atmosphere","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_post_process","description":"Override common settings on a PostProcessVolume: exposureBias, bloomIntensity, vignetteIntensity, colorSaturation [r,g,b,a].","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Optional volume label; defaults to the first PostProcessVolume."},"exposureBias":{"type":"number"},"bloomIntensity":{"type":"number"},"vignetteIntensity":{"type":"number"},"colorSaturation":{"type":"array","items":{"type":"number"}}}},"annotations":{"title":"Set Post Process","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"get_level_info","description":"Summarize the current level: actor count, light count, unique class count, top classes by count, and the aggregate actor-location bounds.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Level Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_world_settings","description":"Read key WorldSettings values: gravity, killZ, worldToMeters, default game mode.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get World Settings","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_map_check_errors","description":"Run editor Map Check and capture log output (errors/warnings about the level). Output may be partial since Map Check also routes to the Message Log.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Map Check Errors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_navigation_info","description":"Report whether a navigation system exists, the nav-data count, and whether navigation is built.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Navigation Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"validate_assets","description":"Run Data Validation over a content path (default /Game/) and capture the output.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Content path. Default /Game/."}}},"annotations":{"title":"Validate Assets","readOnlyHint":true,"openWorldHint":false}},
)JSON")
		TEXT(R"JSON({"name":"create_spline_actor","description":"Spawn an actor with a SplineComponent. Optionally seed 'points' ([[x,y,z],...]) and set a label.","inputSchema":{"type":"object","properties":{"location":{"type":"object","description":"{x,y,z} or [x,y,z]."},"label":{"type":"string"},"points":{"type":"array","items":{"type":"array","items":{"type":"number"}}}}},"annotations":{"title":"Create Spline Actor","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_spline_points","description":"Replace the points of an actor's spline with world-space [[x,y,z],...] and optionally set closedLoop.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Spline actor name or label."},"points":{"type":"array","items":{"type":"array","items":{"type":"number"}}},"closedLoop":{"type":"boolean"}},"required":["name","points"]},"annotations":{"title":"Set Spline Points","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"get_spline_info","description":"Read an actor's spline: point count, closed-loop flag, length, and world-space point locations.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Spline actor name or label."}},"required":["name"]},"annotations":{"title":"Get Spline Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_foliage_stats","description":"Aggregate instanced-foliage in the level: total instance count and per-foliage-type instance counts.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Foliage Stats","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_physics_simulation","description":"Enable or disable rigid-body physics simulation on all primitive components of an actor.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or label."},"enabled":{"type":"boolean"}},"required":["name","enabled"]},"annotations":{"title":"Set Physics Simulation","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_collision_profile","description":"Set the collision profile (e.g. BlockAll, OverlapAll, NoCollision) on all primitive components of an actor.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or label."},"profile":{"type":"string"}},"required":["name","profile"]},"annotations":{"title":"Set Collision Profile","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_collision_enabled","description":"Set collision mode on all primitive components of an actor: NoCollision, QueryOnly, PhysicsOnly, or QueryAndPhysics.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or label."},"mode":{"type":"string"}},"required":["name","mode"]},"annotations":{"title":"Set Collision Enabled","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"play_sound_at_location","description":"Preview a sound asset at a world location in the editor (non-persistent).","inputSchema":{"type":"object","properties":{"soundPath":{"type":"string"},"location":{"type":"object","description":"{x,y,z} or [x,y,z]."},"volume":{"type":"number"},"pitch":{"type":"number"}},"required":["soundPath"]},"annotations":{"title":"Play Sound At Location","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"spawn_ambient_sound","description":"Spawn an AmbientSound actor playing a sound asset at a world location.","inputSchema":{"type":"object","properties":{"soundPath":{"type":"string"},"location":{"type":"object","description":"{x,y,z} or [x,y,z]."},"volume":{"type":"number"},"label":{"type":"string"}},"required":["soundPath"]},"annotations":{"title":"Spawn Ambient Sound","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("set_light_properties")) { OutResult = SetLightProperties(Args); return true; }
	if (ToolName == TEXT("set_fog")) { OutResult = SetFog(Args); return true; }
	if (ToolName == TEXT("set_sky_atmosphere")) { OutResult = SetSkyAtmosphere(Args); return true; }
	if (ToolName == TEXT("set_post_process")) { OutResult = SetPostProcess(Args); return true; }

	if (ToolName == TEXT("get_level_info")) { OutResult = GetLevelInfo(Args); return true; }
	if (ToolName == TEXT("get_world_settings")) { OutResult = GetWorldSettings(Args); return true; }
	if (ToolName == TEXT("get_map_check_errors")) { OutResult = GetMapCheckErrors(Args); return true; }
	if (ToolName == TEXT("get_navigation_info")) { OutResult = GetNavigationInfo(Args); return true; }
	if (ToolName == TEXT("validate_assets")) { OutResult = ValidateAssets(Args); return true; }

	if (ToolName == TEXT("create_spline_actor")) { OutResult = CreateSplineActor(Args); return true; }
	if (ToolName == TEXT("set_spline_points")) { OutResult = SetSplinePoints(Args); return true; }
	if (ToolName == TEXT("get_spline_info")) { OutResult = GetSplineInfo(Args); return true; }
	if (ToolName == TEXT("get_foliage_stats")) { OutResult = GetFoliageStats(Args); return true; }

	if (ToolName == TEXT("set_physics_simulation")) { OutResult = SetPhysicsSimulation(Args); return true; }
	if (ToolName == TEXT("set_collision_profile")) { OutResult = SetCollisionProfile(Args); return true; }
	if (ToolName == TEXT("set_collision_enabled")) { OutResult = SetCollisionEnabled(Args); return true; }
	if (ToolName == TEXT("play_sound_at_location")) { OutResult = PlaySoundAtLocation(Args); return true; }
	if (ToolName == TEXT("spawn_ambient_sound")) { OutResult = SpawnAmbientSound(Args); return true; }

	return false;
}
}
}
