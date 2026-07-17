#include "WorldDataMCPFoliageTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPSpatialTools.h"
#include "WorldDataSceneBriefStore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Math/RandomStream.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace WorldDataMCP
{
namespace FoliageTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
	}

	void AddFoliageInstancesToWorld(UWorld* World, UFoliageType* InFoliageType, const TArray<FTransform>& InTransforms)
	{
		if (!World || !InFoliageType)
		{
			return;
		}

		TMap<AInstancedFoliageActor*, TArray<const FFoliageInstance*>> InstancesToAdd;
		TArray<FFoliageInstance> FoliageInstances;
		FoliageInstances.Reserve(InTransforms.Num());

		for (const FTransform& InstanceTransform : InTransforms)
		{
			AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, true, World->PersistentLevel, InstanceTransform.GetLocation());
			if (!IFA)
			{
				continue;
			}

			FFoliageInstance FoliageInstance;
			FoliageInstance.Location = InstanceTransform.GetLocation();
			FoliageInstance.Rotation = InstanceTransform.GetRotation().Rotator();
			FoliageInstance.DrawScale3D = FVector3f(InstanceTransform.GetScale3D());

			FoliageInstances.Add(FoliageInstance);
			InstancesToAdd.FindOrAdd(IFA).Add(&FoliageInstances.Last());
		}

		for (const auto& Pair : InstancesToAdd)
		{
			FFoliageInfo* TypeInfo = nullptr;
			if (UFoliageType* FoliageType = Pair.Key->AddFoliageType(InFoliageType, &TypeInfo))
			{
				if (TypeInfo)
				{
					TypeInfo->AddInstances(FoliageType, Pair.Value);
				}
			}
		}
	}

	void RemoveAllFoliageInstancesFromWorld(UWorld* World, UFoliageType* InFoliageType)
	{
		if (!World || !InFoliageType)
		{
			return;
		}

		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			if (AInstancedFoliageActor* IFA = *It)
			{
				IFA->RemoveFoliageType(&InFoliageType, 1);
			}
		}
	}

	UObject* LoadAssetObject(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.TrimStartAndEndInline();
		if (Normalized.IsEmpty())
		{
			return nullptr;
		}
		if (!Normalized.Contains(TEXT(".")))
		{
			Normalized = FString::Printf(TEXT("%s.%s"), *Normalized, *FPaths::GetBaseFilename(Normalized));
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Normalized);
	}

	// Reads {x,y,z} (or {pitch,yaw,roll}) from a child object field; returns Fallback when absent.
	FVector ReadVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FVector& Fallback,
		const TCHAR* KX = TEXT("x"), const TCHAR* KY = TEXT("y"), const TCHAR* KZ = TEXT("z"))
	{
		const TSharedPtr<FJsonObject>* VObj = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, VObj) && VObj && (*VObj).IsValid())
		{
			double X = Fallback.X, Y = Fallback.Y, Z = Fallback.Z;
			(*VObj)->TryGetNumberField(KX, X);
			(*VObj)->TryGetNumberField(KY, Y);
			(*VObj)->TryGetNumberField(KZ, Z);
			return FVector(X, Y, Z);
		}
		return Fallback;
	}

	// Applies a flat {name:value} settings object onto a UObject via property reflection.
	// Returns the names that were applied; appends failures to OutFailed.
	TArray<FString> ApplyReflectedSettings(UObject* Target, const TSharedPtr<FJsonObject>& Settings, TArray<FString>& OutFailed)
	{
		TArray<FString> Applied;
		if (!Target || !Settings.IsValid())
		{
			return Applied;
		}
		for (const auto& KV : Settings->Values)
		{
			const FString PropName(KV.Key);
			FString PropValue;
			switch (KV.Value->Type)
			{
			case EJson::String:  PropValue = KV.Value->AsString(); break;
			case EJson::Number:  PropValue = FString::SanitizeFloat(KV.Value->AsNumber()); break;
			case EJson::Boolean: PropValue = KV.Value->AsBool() ? TEXT("True") : TEXT("False"); break;
			default:             PropValue = KV.Value->AsString(); break;
			}
			FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Property)
			{
				OutFailed.Add(FString::Printf(TEXT("%s: property not found"), *PropName));
				continue;
			}
			void* Addr = Property->ContainerPtrToValuePtr<void>(Target);
			const TCHAR* ImportResult = Property->ImportText_Direct(*PropValue, Addr, Target, PPF_None);
			if (ImportResult == nullptr)
			{
				OutFailed.Add(FString::Printf(TEXT("%s: failed to set '%s'"), *PropName, *PropValue));
			}
			else
			{
				Applied.Add(PropName);
			}
		}
		return Applied;
	}

	// Resolve a UFoliageType from an explicit asset path, or — if only a mesh is given —
	// find-or-create a SAVED UFoliageType_InstancedStaticMesh asset for that mesh.
	// NOTE: we deliberately do NOT call AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel
	// / AddMesh here: those assert (!IsPartitionedWorld) and hard-crash the editor on World Partition
	// maps. The static AInstancedFoliageActor::AddInstances used by the caller is WP-safe and creates
	// the per-partition foliage info itself, so it only needs a valid (persistable) UFoliageType.
	UFoliageType* ResolveFoliageType(const TSharedPtr<FJsonObject>& Args, UWorld* World, FString& OutError)
	{
		FString FoliageTypePath;
		Args->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);
		if (!FoliageTypePath.IsEmpty())
		{
			UFoliageType* FoliageType = Cast<UFoliageType>(LoadAssetObject(FoliageTypePath));
			if (!FoliageType)
			{
				OutError = FString::Printf(TEXT("Foliage type '%s' not found."), *FoliageTypePath);
			}
			return FoliageType;
		}

		FString MeshPath;
		Args->TryGetStringField(TEXT("meshPath"), MeshPath);
		if (MeshPath.IsEmpty())
		{
			OutError = TEXT("Provide either 'foliageTypePath' or 'meshPath'.");
			return nullptr;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(LoadAssetObject(MeshPath));
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("Static mesh '%s' not found."), *MeshPath);
			return nullptr;
		}
		// Find-or-create a saved foliage-type asset (no IFA involvement → WP-safe).
		const FString AssetName = FString::Printf(TEXT("FT_%s"), *Mesh->GetName());
		const FString FtPath = FString::Printf(TEXT("/Game/Foliage/%s"), *AssetName);
		if (UFoliageType* Existing = Cast<UFoliageType>(LoadAssetObject(FtPath)))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*FtPath);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *FtPath);
			return nullptr;
		}
		UFoliageType_InstancedStaticMesh* FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(
			Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!FoliageType)
		{
			OutError = TEXT("Failed to create a foliage type for the mesh.");
			return nullptr;
		}
		FoliageType->Mesh = Mesh;
		FAssetRegistryModule::AssetCreated(FoliageType);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(FtPath, /*bOnlyIfIsDirty*/false);
		return FoliageType;
	}

	// ---- tools ---------------------------------------------------------------------------

	FString CreateFoliageType(const TSharedPtr<FJsonObject>& Args)
	{
		FString MeshPath;
		Args->TryGetStringField(TEXT("meshPath"), MeshPath);
		UStaticMesh* Mesh = Cast<UStaticMesh>(LoadAssetObject(MeshPath));
		if (!Mesh)
		{
			return ErrorJson(FString::Printf(TEXT("Static mesh '%s' not found."), *MeshPath));
		}

		FString AssetName;
		Args->TryGetStringField(TEXT("name"), AssetName);
		if (AssetName.IsEmpty())
		{
			AssetName = FString::Printf(TEXT("FT_%s"), *Mesh->GetName());
		}
		FString PackagePath = TEXT("/Game/Foliage");
		Args->TryGetStringField(TEXT("packagePath"), PackagePath);

		const FString PackageFullPath = PackagePath / AssetName;
		if (UEditorAssetLibrary::DoesAssetExist(PackageFullPath))
		{
			return ErrorJson(FString::Printf(TEXT("Asset already exists: %s"), *PackageFullPath));
		}
		UPackage* Package = CreatePackage(*PackageFullPath);
		if (!Package)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create package: %s"), *PackageFullPath));
		}
		UFoliageType_InstancedStaticMesh* FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(
			Package, *AssetName, RF_Public | RF_Standalone);
		if (!FoliageType)
		{
			return ErrorJson(TEXT("Failed to create UFoliageType_InstancedStaticMesh."));
		}
		FoliageType->Mesh = Mesh;

		TArray<FString> Failed;
		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (Args->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && (*SettingsObj).IsValid())
		{
			ApplyReflectedSettings(FoliageType, *SettingsObj, Failed);
		}

		FAssetRegistryModule::AssetCreated(FoliageType);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(PackageFullPath, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), FoliageType->GetPathName());
		Result->SetStringField(TEXT("name"), FoliageType->GetName());
		Result->SetStringField(TEXT("meshPath"), Mesh->GetPathName());
		if (Failed.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FailedArr;
			for (const FString& F : Failed) { FailedArr.Add(MakeShared<FJsonValueString>(F)); }
			Result->SetArrayField(TEXT("failedSettings"), FailedArr);
		}
		return SuccessJson(Result);
	}

	FString ListFoliageTypes(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("No editor world available."));
		}
		TArray<TSharedPtr<FJsonValue>> TypesArr;
		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			AInstancedFoliageActor* IFA = *It;
			if (!IFA) { continue; }
			for (const auto& Pair : IFA->GetFoliageInfos())
			{
				UFoliageType* FoliageType = Pair.Key;
				if (!FoliageType) { continue; }
				int32 InstanceCount = 0;
				if (UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value->GetComponent())
				{
					InstanceCount = HISM->GetInstanceCount();
				}
				TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetStringField(TEXT("name"), FoliageType->GetName());
				E->SetStringField(TEXT("path"), FoliageType->GetPathName());
				E->SetStringField(TEXT("className"), FoliageType->GetClass()->GetName());
				E->SetNumberField(TEXT("instanceCount"), InstanceCount);
				if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(FoliageType))
				{
					if (ISM->Mesh) { E->SetStringField(TEXT("meshPath"), ISM->Mesh->GetPathName()); }
				}
				TypesArr.Add(MakeShared<FJsonValueObject>(E));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), TypesArr.Num());
		Result->SetArrayField(TEXT("foliageTypes"), TypesArr);
		return SuccessJson(Result);
	}

	FString AddFoliageInstances(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("No editor world available."));
		}
		FString ResolveError;
		// Scene-generation gate: scattering foliage is content placement.
			{
				FString GateReason;
				if (!::WorldDataMCP::HasActiveSceneBrief(World->GetMapName(), GateReason))
				{
					return ErrorJson(GateReason);
				}
			}

			UFoliageType* FoliageType = ResolveFoliageType(Args, World, ResolveError);
		if (!FoliageType)
		{
			return ErrorJson(ResolveError);
		}

		TArray<FTransform> Transforms;

		const TArray<TSharedPtr<FJsonValue>>* ExplicitArr = nullptr;
		if (Args->TryGetArrayField(TEXT("transforms"), ExplicitArr) && ExplicitArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *ExplicitArr)
			{
				const TSharedPtr<FJsonObject>* TObj = nullptr;
				if (!V->TryGetObject(TObj) || !(*TObj).IsValid()) { continue; }
				FVector Loc = ReadVec(*TObj, TEXT("location"), FVector::ZeroVector);
				FVector Scale = ReadVec(*TObj, TEXT("scale"), FVector::OneVector);
				FVector RotV = ReadVec(*TObj, TEXT("rotation"), FVector::ZeroVector, TEXT("pitch"), TEXT("yaw"), TEXT("roll"));
				Transforms.Add(FTransform(FRotator(RotV.X, RotV.Y, RotV.Z), Loc, Scale));
			}
		}
		else
		{
			// Procedural scatter. Region = center+radius (circle) or boxMin/boxMax.
			int32 Count = 0;
			{
				double CountNum = 0;
				Args->TryGetNumberField(TEXT("count"), CountNum);
				Count = FMath::Clamp(static_cast<int32>(CountNum), 0, 100000);
			}
			if (Count <= 0)
			{
				return ErrorJson(TEXT("Provide a 'transforms' array, or a procedural 'count' (>0) with a region."));
			}
			double ScaleMin = 1.0, ScaleMax = 1.0;
			Args->TryGetNumberField(TEXT("scaleMin"), ScaleMin);
			Args->TryGetNumberField(TEXT("scaleMax"), ScaleMax);
			if (ScaleMax < ScaleMin) { ScaleMax = ScaleMin; }
			bool bRandomYaw = true;
			Args->TryGetBoolField(TEXT("randomYaw"), bRandomYaw);
			bool bAlignToGround = true;
			Args->TryGetBoolField(TEXT("alignToGround"), bAlignToGround);
			double TraceUp = 10000.0, TraceLen = 20000.0;
			Args->TryGetNumberField(TEXT("traceUpOffset"), TraceUp);
			Args->TryGetNumberField(TEXT("traceLength"), TraceLen);
			int32 Seed = 1337;
			{
				double SeedNum = 1337;
				Args->TryGetNumberField(TEXT("seed"), SeedNum);
				Seed = static_cast<int32>(SeedNum);
			}
			FRandomStream Stream(Seed);

			// Region setup.
			const TSharedPtr<FJsonObject>* CenterObj = nullptr;
			const bool bHasCircle = Args->TryGetObjectField(TEXT("center"), CenterObj) && CenterObj && (*CenterObj).IsValid();
			FVector Center = ReadVec(Args, TEXT("center"), FVector::ZeroVector);
			double Radius = 0.0;
			Args->TryGetNumberField(TEXT("radius"), Radius);
			FVector BoxMin = ReadVec(Args, TEXT("boxMin"), FVector::ZeroVector);
			FVector BoxMax = ReadVec(Args, TEXT("boxMax"), FVector::ZeroVector);
			const bool bHasBox = !BoxMin.Equals(BoxMax);
			if (!bHasBox && !(bHasCircle && Radius > 0.0))
			{
				return ErrorJson(TEXT("Procedural scatter needs either center+radius or boxMin+boxMax."));
			}

			FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(WorldDataFoliageScatter), /*bTraceComplex*/true);
			int32 Skipped = 0;
			for (int32 i = 0; i < Count; ++i)
			{
				FVector P;
				if (bHasBox)
				{
					P.X = Stream.FRandRange(BoxMin.X, BoxMax.X);
					P.Y = Stream.FRandRange(BoxMin.Y, BoxMax.Y);
					P.Z = Stream.FRandRange(BoxMin.Z, BoxMax.Z);
				}
				else
				{
					const float Angle = Stream.FRandRange(0.f, 2.f * PI);
					const float Dist = Radius * FMath::Sqrt(Stream.FRand());
					P = Center + FVector(Dist * FMath::Cos(Angle), Dist * FMath::Sin(Angle), 0.0);
				}

				FRotator Rot = FRotator::ZeroRotator;
				if (bAlignToGround)
				{
					const FVector Start(P.X, P.Y, P.Z + TraceUp);
					const FVector End(P.X, P.Y, P.Z + TraceUp - TraceLen);
					FHitResult Hit;
					if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, TraceParams))
					{
						P = Hit.ImpactPoint;
					}
					else
					{
						++Skipped;
						continue;
					}
				}
				if (bRandomYaw)
				{
					Rot.Yaw = Stream.FRandRange(0.f, 360.f);
				}
				const double S = Stream.FRandRange(ScaleMin, ScaleMax);
				Transforms.Add(FTransform(Rot, P, FVector(S)));
			}

			if (Transforms.Num() == 0)
			{
				return ErrorJson(FString::Printf(TEXT("No instances placed (all %d trace points missed the ground)."), Skipped));
			}
		}

		// A2 cross-path prune: optionally drop scattered instances that overlap ANY existing placed
		// content — PCG-ISM, other foliage, plain ISM, or actors — so this route stops interpenetrating
		// the others. Reads the unified placement model. Default OFF to preserve existing behaviour.
		bool bAvoidExisting = false;
		Args->TryGetBoolField(TEXT("avoidExisting"), bAvoidExisting);
		int32 SkippedOverlap = 0;
		if (bAvoidExisting && Transforms.Num() > 0)
		{
			double ClearanceRadius = 0.0;
			Args->TryGetNumberField(TEXT("clearanceRadius"), ClearanceRadius);
			if (ClearanceRadius <= 0.0)
			{
				// Default the clearance to the foliage mesh's horizontal footprint.
				if (UFoliageType_InstancedStaticMesh* FTISM = Cast<UFoliageType_InstancedStaticMesh>(FoliageType))
				{
					if (UStaticMesh* FMesh = FTISM->GetStaticMesh())
					{
						const FVector Ext = FMesh->GetBounds().BoxExtent;
						ClearanceRadius = FMath::Max(Ext.X, Ext.Y);
					}
				}
			}
			if (ClearanceRadius <= 0.0)
			{
				ClearanceRadius = 100.0;
			}

			// Bound the obstacle gather to the scatter area (+ margin) so cost stays local.
			FBox ScatterRegion(ForceInit);
			for (const FTransform& Xf : Transforms)
			{
				ScatterRegion += Xf.GetLocation();
			}
			if (ScatterRegion.IsValid)
			{
				ScatterRegion = ScatterRegion.ExpandBy(FVector(ClearanceRadius * 2.0));
			}

			TArray<FBox> Obstacles;
			WorldDataMCP::SpatialTools::GatherPlacementBounds(World, Obstacles, ScatterRegion);

			TArray<FTransform> Kept;
			Kept.Reserve(Transforms.Num());
			for (const FTransform& Xf : Transforms)
			{
				const double R = ClearanceRadius * FMath::Max3(Xf.GetScale3D().X, Xf.GetScale3D().Y, 1.0);
				const FVector L = Xf.GetLocation();
				const FBox Cand(L - FVector(R, R, R), L + FVector(R, R, R));
				bool bBlocked = false;
				for (const FBox& Obstacle : Obstacles)
				{
					if (Obstacle.Intersect(Cand))
					{
						bBlocked = true;
						break;
					}
				}
				if (bBlocked)
				{
					++SkippedOverlap;
				}
				else
				{
					Kept.Add(Xf);
				}
			}
			Transforms = MoveTemp(Kept);
			if (Transforms.Num() == 0)
			{
				return ErrorJson(FString::Printf(TEXT("All %d instances pruned as overlapping existing content (avoidExisting). Lower clearanceRadius or pick a clearer area."), SkippedOverlap));
			}
		}

		AddFoliageInstancesToWorld(World, FoliageType, Transforms);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("foliageType"), FoliageType->GetPathName());
		Result->SetNumberField(TEXT("addedCount"), Transforms.Num());
		Result->SetNumberField(TEXT("skippedOverlap"), SkippedOverlap);
		return SuccessJson(Result);
	}

	FString RemoveAllFoliageInstances(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("No editor world available."));
		}
		FString FoliageTypePath;
		Args->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);
		UFoliageType* FoliageType = Cast<UFoliageType>(LoadAssetObject(FoliageTypePath));
		if (!FoliageType)
		{
			// Fall back to a level-embedded type matched by name/path.
			for (TActorIterator<AInstancedFoliageActor> It(World); It && !FoliageType; ++It)
			{
				if (!*It) { continue; }
				for (const auto& Pair : (*It)->GetFoliageInfos())
				{
					if (Pair.Key && (Pair.Key->GetName() == FoliageTypePath || Pair.Key->GetPathName() == FoliageTypePath))
					{
						FoliageType = Pair.Key;
						break;
					}
				}
			}
		}
		if (!FoliageType)
		{
			return ErrorJson(FString::Printf(TEXT("Foliage type '%s' not found."), *FoliageTypePath));
		}
		RemoveAllFoliageInstancesFromWorld(World, FoliageType);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("foliageType"), FoliageType->GetPathName());
		return SuccessJson(Result);
	}

	FString GetFoliageTypeSettings(const TSharedPtr<FJsonObject>& Args)
	{
		FString FoliageTypePath;
		Args->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);
		UFoliageType* FoliageType = Cast<UFoliageType>(LoadAssetObject(FoliageTypePath));
		if (!FoliageType)
		{
			return ErrorJson(FString::Printf(TEXT("Foliage type '%s' not found."), *FoliageTypePath));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), FoliageType->GetPathName());
		Result->SetStringField(TEXT("name"), FoliageType->GetName());
		Result->SetNumberField(TEXT("density"), FoliageType->Density);
		Result->SetNumberField(TEXT("radius"), FoliageType->Radius);
		Result->SetNumberField(TEXT("scaleMinX"), FoliageType->ScaleX.Min);
		Result->SetNumberField(TEXT("scaleMaxX"), FoliageType->ScaleX.Max);
		Result->SetBoolField(TEXT("alignToNormal"), FoliageType->AlignToNormal);
		Result->SetBoolField(TEXT("randomYaw"), FoliageType->RandomYaw);
		Result->SetNumberField(TEXT("groundSlopeAngleMax"), FoliageType->GroundSlopeAngle.Max);
		Result->SetNumberField(TEXT("cullDistanceMax"), FoliageType->CullDistance.Max);
		Result->SetBoolField(TEXT("castShadow"), FoliageType->CastShadow);
		if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(FoliageType))
		{
			if (ISM->Mesh) { Result->SetStringField(TEXT("meshPath"), ISM->Mesh->GetPathName()); }
		}
		return SuccessJson(Result);
	}

	FString SetFoliageTypeSettings(const TSharedPtr<FJsonObject>& Args)
	{
		FString FoliageTypePath;
		Args->TryGetStringField(TEXT("foliageTypePath"), FoliageTypePath);
		UFoliageType* FoliageType = Cast<UFoliageType>(LoadAssetObject(FoliageTypePath));
		if (!FoliageType)
		{
			return ErrorJson(FString::Printf(TEXT("Foliage type '%s' not found."), *FoliageTypePath));
		}
		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (!Args->TryGetObjectField(TEXT("settings"), SettingsObj) || !SettingsObj || !(*SettingsObj).IsValid())
		{
			return ErrorJson(TEXT("Missing 'settings' object (flat name/value pairs)."));
		}
		TArray<FString> Failed;
		TArray<FString> Applied = ApplyReflectedSettings(FoliageType, *SettingsObj, Failed);
		FoliageType->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(FoliageType, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), FoliageType->GetPathName());
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) { AppliedArr.Add(MakeShared<FJsonValueString>(A)); }
		Result->SetArrayField(TEXT("appliedSettings"), AppliedArr);
		if (Failed.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FailedArr;
			for (const FString& F : Failed) { FailedArr.Add(MakeShared<FJsonValueString>(F)); }
			Result->SetArrayField(TEXT("failedSettings"), FailedArr);
		}
		Result->SetBoolField(TEXT("allApplied"), Failed.Num() == 0);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_foliage_type","description":"Create a UFoliageType_InstancedStaticMesh asset from a static mesh. Optional 'settings' (flat name/value pairs) applied via reflection.","inputSchema":{"type":"object","properties":{"meshPath":{"type":"string","description":"Static mesh asset path."},"name":{"type":"string","description":"Asset name (default FT_<mesh>)."},"packagePath":{"type":"string","description":"Package folder (default /Game/Foliage)."},"settings":{"type":"object","description":"Optional UFoliageType properties to set (e.g. Density, Radius)."}},"required":["meshPath"]},"annotations":{"title":"Create Foliage Type","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"list_foliage_types","description":"List foliage types painted into the current editor level (name, path, mesh, instance count).","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Foliage Types","readOnlyHint":true,"openWorldHint":false}},
{"name":"add_foliage_instances","description":"Paint foliage instances into the level. Give either 'foliageTypePath' or 'meshPath' (find-or-creates a level foliage type). Then either an explicit 'transforms' array ([{location:{x,y,z},rotation:{pitch,yaw,roll},scale:{x,y,z}}]) OR procedural scatter: 'count' plus a region (center+radius OR boxMin+boxMax), with optional scaleMin/scaleMax, randomYaw, alignToGround (line-traces onto ECC_WorldStatic, default true), traceUpOffset, traceLength, seed. Set avoidExisting to prune scattered instances that overlap existing scene content (cross-path; reads the unified placement model so it also avoids non-collidable PCG/foliage instances).","inputSchema":{"type":"object","properties":{"foliageTypePath":{"type":"string"},"meshPath":{"type":"string"},"transforms":{"type":"array"},"count":{"type":"number"},"center":{"type":"object"},"radius":{"type":"number"},"boxMin":{"type":"object"},"boxMax":{"type":"object"},"scaleMin":{"type":"number"},"scaleMax":{"type":"number"},"randomYaw":{"type":"boolean"},"alignToGround":{"type":"boolean"},"traceUpOffset":{"type":"number"},"traceLength":{"type":"number"},"seed":{"type":"number"},"avoidExisting":{"type":"boolean","description":"Cross-path prune: drop scattered instances overlapping ANY existing placed content (PCG-ISM, other foliage, plain ISM, actors) so this scatter stops interpenetrating the other routes. Default false."},"clearanceRadius":{"type":"number","description":"Min clearance in cm when avoidExisting is on. Default = the foliage mesh's horizontal footprint."}}},"annotations":{"title":"Add Foliage Instances","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"remove_all_foliage_instances","description":"Remove all painted instances of a foliage type from the current level (accepts asset path or a level-embedded type name).","inputSchema":{"type":"object","properties":{"foliageTypePath":{"type":"string"}},"required":["foliageTypePath"]},"annotations":{"title":"Remove All Foliage Instances","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"get_foliage_type_settings","description":"Read common settings of a foliage type asset (density, radius, scale range, alignToNormal, randomYaw, slope, cull distance, cast shadow, mesh).","inputSchema":{"type":"object","properties":{"foliageTypePath":{"type":"string"}},"required":["foliageTypePath"]},"annotations":{"title":"Get Foliage Type Settings","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_foliage_type_settings","description":"Set foliage type properties via reflection ('settings' = flat name/value pairs) and save the asset.","inputSchema":{"type":"object","properties":{"foliageTypePath":{"type":"string"},"settings":{"type":"object"}},"required":["foliageTypePath","settings"]},"annotations":{"title":"Set Foliage Type Settings","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_foliage_type")) { OutResult = CreateFoliageType(Args); return true; }
	if (ToolName == TEXT("list_foliage_types")) { OutResult = ListFoliageTypes(Args); return true; }
	if (ToolName == TEXT("add_foliage_instances")) { OutResult = AddFoliageInstances(Args); return true; }
	if (ToolName == TEXT("remove_all_foliage_instances")) { OutResult = RemoveAllFoliageInstances(Args); return true; }
	if (ToolName == TEXT("get_foliage_type_settings")) { OutResult = GetFoliageTypeSettings(Args); return true; }
	if (ToolName == TEXT("set_foliage_type_settings")) { OutResult = SetFoliageTypeSettings(Args); return true; }
	return false;
}
}
}
