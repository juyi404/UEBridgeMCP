#include "WorldDataMCPTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPServer.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FString NormalizeAssetObjectPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return Path;
		}

		if (Path.Contains(TEXT(".")))
		{
			return Path;
		}

		const FString AssetName = FPaths::GetBaseFilename(Path);
		return FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
	}

	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return GWorld;
	}

	void SetVectorFields(const TSharedRef<FJsonObject>& Json, const FVector& Vector)
	{
		Json->SetNumberField(TEXT("x"), Vector.X);
		Json->SetNumberField(TEXT("y"), Vector.Y);
		Json->SetNumberField(TEXT("z"), Vector.Z);
	}

	TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Vector)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		SetVectorFields(Json, Vector);
		return Json;
	}

	TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Rotator)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Json->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Json->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Json;
	}

	bool TryGetNumberFieldCaseInsensitive(const TSharedPtr<FJsonObject>& Json, const TCHAR* LowerName, const TCHAR* UpperName, double& OutValue)
	{
		return Json->TryGetNumberField(LowerName, OutValue) || Json->TryGetNumberField(UpperName, OutValue);
	}

	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, FVector& OutVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Args->TryGetArrayField(FieldName, Array) && Array && Array->Num() >= 3)
		{
			OutVector.X = (*Array)[0]->AsNumber();
			OutVector.Y = (*Array)[1]->AsNumber();
			OutVector.Z = (*Array)[2]->AsNumber();
			return true;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Args->TryGetObjectField(FieldName, Object) && Object && Object->IsValid())
		{
			double X = OutVector.X;
			double Y = OutVector.Y;
			double Z = OutVector.Z;
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("x"), TEXT("X"), X);
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("y"), TEXT("Y"), Y);
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("z"), TEXT("Z"), Z);
			OutVector = FVector(X, Y, Z);
			return true;
		}

		return false;
	}

	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, FRotator& OutRotator)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Args->TryGetArrayField(FieldName, Array) && Array && Array->Num() >= 3)
		{
			OutRotator = FRotator((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
			return true;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Args->TryGetObjectField(FieldName, Object) && Object && Object->IsValid())
		{
			double Pitch = OutRotator.Pitch;
			double Yaw = OutRotator.Yaw;
			double Roll = OutRotator.Roll;
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("pitch"), TEXT("Pitch"), Pitch);
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("yaw"), TEXT("Yaw"), Yaw);
			TryGetNumberFieldCaseInsensitive(*Object, TEXT("roll"), TEXT("Roll"), Roll);
			OutRotator = FRotator(Pitch, Yaw, Roll);
			return true;
		}

		return false;
	}

	AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}

			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		return nullptr;
	}

	UClass* ResolveActorClass(const FString& ClassText)
	{
		if (ClassText.IsEmpty())
		{
			return AActor::StaticClass();
		}

		UClass* ActorClass = FindObject<UClass>(nullptr, *ClassText);
		if (!ActorClass)
		{
			ActorClass = LoadObject<UClass>(nullptr, *ClassText);
		}

		if (!ActorClass && !ClassText.StartsWith(TEXT("/")))
		{
			const FString EngineClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassText);
			ActorClass = FindObject<UClass>(nullptr, *EngineClassPath);
		}

		if (!ActorClass && ClassText.StartsWith(TEXT("/")))
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(ClassText));
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				ActorClass = Blueprint->GeneratedClass;
			}
		}

		return ActorClass && ActorClass->IsChildOf(AActor::StaticClass()) ? ActorClass : nullptr;
	}

	FString MobilityToString(EComponentMobility::Type Mobility)
	{
		switch (Mobility)
		{
		case EComponentMobility::Static: return TEXT("Static");
		case EComponentMobility::Stationary: return TEXT("Stationary");
		case EComponentMobility::Movable: return TEXT("Movable");
		default: return TEXT("Unknown");
		}
	}

	int32 CountActorComponents(AActor* Actor)
	{
		if (!IsValid(Actor))
		{
			return 0;
		}
		TInlineComponentArray<UActorComponent*> Components(Actor);
		return Components.Num();
	}

	TSharedPtr<FJsonObject> MakeActorObject(AActor* Actor)
	{
		TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
		if (!IsValid(Actor))
		{
			return ActorJson;
		}

		ActorJson->SetStringField(TEXT("name"), Actor->GetName());
		ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		ActorJson->SetStringField(TEXT("path"), Actor->GetPathName());

		const FName FolderPath = Actor->GetFolderPath();
		ActorJson->SetStringField(TEXT("folderPath"), FolderPath.IsNone() ? FString() : FolderPath.ToString());

		ActorJson->SetObjectField(TEXT("location"), MakeVectorObject(Actor->GetActorLocation()));
		ActorJson->SetObjectField(TEXT("rotation"), MakeRotatorObject(Actor->GetActorRotation()));
		ActorJson->SetObjectField(TEXT("scale"), MakeVectorObject(Actor->GetActorScale3D()));

		if (const USceneComponent* Root = Actor->GetRootComponent())
		{
			ActorJson->SetStringField(TEXT("mobility"), MobilityToString(Root->Mobility.GetValue()));
		}

		ActorJson->SetNumberField(TEXT("componentCount"), CountActorComponents(Actor));
		ActorJson->SetBoolField(TEXT("selected"), Actor->IsSelected());

		if (Actor->Tags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Tags;
			for (const FName& Tag : Actor->Tags)
			{
				Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			ActorJson->SetArrayField(TEXT("tags"), Tags);
		}

		return ActorJson;
	}

	struct FContentCountsCacheEntry
	{
		double CachedAtSeconds = 0.0;
		int32 TotalAssets = 0;
		TMap<FString, int32> CountsByClass;
	};

	// Game-thread-only cache. The Asset Registry survey is O(all assets under path),
	// so a short TTL keeps repeated bootstrap/content_summary calls cheap without
	// returning stale data for long.
	static TMap<FString, FContentCountsCacheEntry> GContentCountsCache;
	static constexpr double GContentCountsCacheTtlSeconds = 15.0;

	void ComputeContentCounts(const FString& SearchRoot, int32& OutTotalAssets, TMap<FString, int32>& OutCountsByClass)
	{
		const double Now = FPlatformTime::Seconds();
		if (const FContentCountsCacheEntry* Cached = GContentCountsCache.Find(SearchRoot))
		{
			if (Now - Cached->CachedAtSeconds < GContentCountsCacheTtlSeconds)
			{
				OutTotalAssets = Cached->TotalAssets;
				OutCountsByClass = Cached->CountsByClass;
				return;
			}
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*SearchRoot));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssets(Filter, AssetDataList);

		OutCountsByClass.Reset();
		for (const FAssetData& AssetData : AssetDataList)
		{
			OutCountsByClass.FindOrAdd(AssetData.AssetClassPath.GetAssetName().ToString())++;
		}
		OutTotalAssets = AssetDataList.Num();

		FContentCountsCacheEntry& Entry = GContentCountsCache.FindOrAdd(SearchRoot);
		Entry.CachedAtSeconds = Now;
		Entry.TotalAssets = OutTotalAssets;
		Entry.CountsByClass = OutCountsByClass;
	}
}

namespace WorldDataMCP
{
	namespace Tools
	{
		FString ListLevelActors(const TSharedPtr<FJsonObject>& Args)
		{
			UWorld* World = GetEditorWorld();
			if (!World)
			{
				return ErrorJson(TEXT("Editor world is not available."));
			}

			FString ClassFilter;
			Args->TryGetStringField(TEXT("classFilter"), ClassFilter);

			FString NameContains;
			Args->TryGetStringField(TEXT("nameContains"), NameContains);

			bool bSelectedOnly = false;
			Args->TryGetBoolField(TEXT("selectedOnly"), bSelectedOnly);

			double MaxResultsNumber = 200.0;
			Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
			const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 1000);

			TArray<TSharedPtr<FJsonValue>> Actors;
			int32 MatchedCount = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor))
				{
					continue;
				}

				if (bSelectedOnly && !Actor->IsSelected())
				{
					continue;
				}

				const FString ClassName = Actor->GetClass()->GetName();
				const FString Label = Actor->GetActorLabel();
				if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!NameContains.IsEmpty()
					&& !Label.Contains(NameContains, ESearchCase::IgnoreCase)
					&& !Actor->GetName().Contains(NameContains, ESearchCase::IgnoreCase))
				{
					continue;
				}

				++MatchedCount;
				if (Actors.Num() < MaxResults)
				{
					Actors.Add(MakeShared<FJsonValueObject>(MakeActorObject(Actor)));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetNumberField(TEXT("matchedCount"), MatchedCount);
			Result->SetBoolField(TEXT("truncated"), MatchedCount > Actors.Num());
			Result->SetArrayField(TEXT("actors"), Actors);
			return SuccessJson(Result);
		}

		FString FindAssets(const TSharedPtr<FJsonObject>& Args)
		{
			FString SearchTerm;
			Args->TryGetStringField(TEXT("searchTerm"), SearchTerm);

			FString SearchRoot = TEXT("/Game");
			Args->TryGetStringField(TEXT("path"), SearchRoot);
			if (SearchRoot.IsEmpty())
			{
				SearchRoot = TEXT("/Game");
			}

			FString ClassFilter;
			Args->TryGetStringField(TEXT("classFilter"), ClassFilter);

			double MaxResultsNumber = 50.0;
			Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
			const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 500);

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*SearchRoot));
			Filter.bRecursivePaths = true;

			// When classFilter resolves to a concrete UClass, push it into the registry
			// query (indexed) instead of scanning every asset and substring-matching in C++.
			bool bClassPushedDown = false;
			if (!ClassFilter.IsEmpty())
			{
				if (const UClass* ResolvedClass = UClass::TryFindTypeSlow<UClass>(ClassFilter))
				{
					Filter.ClassPaths.Add(ResolvedClass->GetClassPathName());
					Filter.bRecursiveClasses = true;
					bClassPushedDown = true;
				}
			}

			TArray<FAssetData> AssetDataList;
			AssetRegistry.GetAssets(Filter, AssetDataList);

			TArray<TSharedPtr<FJsonValue>> Assets;
			int32 MatchedCount = 0;
			for (const FAssetData& AssetData : AssetDataList)
			{
				const FString AssetName = AssetData.AssetName.ToString();
				const FString ObjectPath = AssetData.GetObjectPathString();
				const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

				if (!SearchTerm.IsEmpty()
					&& !AssetName.Contains(SearchTerm, ESearchCase::IgnoreCase)
					&& !ObjectPath.Contains(SearchTerm, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!bClassPushedDown && !ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}

				++MatchedCount;
				if (Assets.Num() < MaxResults)
				{
					TSharedRef<FJsonObject> AssetJson = MakeShared<FJsonObject>();
					AssetJson->SetStringField(TEXT("name"), AssetName);
					AssetJson->SetStringField(TEXT("path"), ObjectPath);
					AssetJson->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
					AssetJson->SetStringField(TEXT("class"), ClassName);
					Assets.Add(MakeShared<FJsonValueObject>(AssetJson));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Assets.Num());
			Result->SetNumberField(TEXT("matchedCount"), MatchedCount);
			Result->SetBoolField(TEXT("truncated"), MatchedCount > Assets.Num());
			Result->SetArrayField(TEXT("assets"), Assets);
			return SuccessJson(Result);
		}

		FString ReadAsset(const TSharedPtr<FJsonObject>& Args)
		{
			FString Path;
			if (!Args->TryGetStringField(TEXT("assetPath"), Path) && !Args->TryGetStringField(TEXT("path"), Path))
			{
				return ErrorJson(TEXT("Missing required field 'assetPath'."));
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

			FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizeAssetObjectPath(Path)));
			if (!AssetData.IsValid())
			{
				FString PackageName = Path;
				if (PackageName.Contains(TEXT(".")))
				{
					PackageName.Split(TEXT("."), &PackageName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				}

				TArray<FAssetData> PackageAssets;
				AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets);
				if (PackageAssets.Num() > 0)
				{
					AssetData = PackageAssets[0];
				}
			}

			if (!AssetData.IsValid())
			{
				return ErrorJson(FString::Printf(TEXT("Asset not found: %s"), *Path));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
			Result->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
			Result->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
			Result->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
			Result->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
			Result->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());
			Result->SetBoolField(TEXT("isRedirector"), AssetData.IsRedirector());
			return SuccessJson(Result);
		}

		FString SelectActor(const TSharedPtr<FJsonObject>& Args)
		{
			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Actor = FindActorByNameOrLabel(World, Name);
			if (!Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Actor, true, true);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString SpawnActor(const TSharedPtr<FJsonObject>& Args)
		{
			UWorld* World = GetEditorWorld();
			if (!World)
			{
				return ErrorJson(TEXT("Editor world is not available."));
			}

			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale = FVector::OneVector;
			TryGetVectorField(Args, TEXT("location"), Location);
			TryGetRotatorField(Args, TEXT("rotation"), Rotation);
			TryGetVectorField(Args, TEXT("scale"), Scale);

			FString Label;
			Args->TryGetStringField(TEXT("label"), Label);

			FString StaticMeshPath;
			Args->TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath);

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "SpawnActor", "MCP Spawn Actor"));
			World->Modify();

			AActor* Actor = nullptr;
			if (!StaticMeshPath.IsEmpty())
			{
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *NormalizeAssetObjectPath(StaticMeshPath));
				if (!Mesh)
				{
					return ErrorJson(FString::Printf(TEXT("Static mesh not found: %s"), *StaticMeshPath));
				}

				AStaticMeshActor* StaticMeshActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation);
				if (!StaticMeshActor)
				{
					return ErrorJson(TEXT("Failed to spawn static mesh actor."));
				}

				StaticMeshActor->Modify();
				StaticMeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor = StaticMeshActor;
			}
			else
			{
				FString ClassText = TEXT("Actor");
				Args->TryGetStringField(TEXT("class"), ClassText);

				UClass* ActorClass = ResolveActorClass(ClassText);
				if (!ActorClass)
				{
					return ErrorJson(FString::Printf(TEXT("Actor class not found or not spawnable: %s"), *ClassText));
				}

				Actor = World->SpawnActor<AActor>(ActorClass, Location, Rotation);
				if (!Actor)
				{
					return ErrorJson(FString::Printf(TEXT("Failed to spawn actor of class: %s"), *ActorClass->GetName()));
				}
				Actor->Modify();
			}

			Actor->SetActorScale3D(Scale);
			if (!Label.IsEmpty())
			{
				Actor->SetActorLabel(Label);
			}

			World->MarkPackageDirty();
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Actor, true, true);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString GetSelectedActors(const TSharedPtr<FJsonObject>& Args)
		{
			TArray<TSharedPtr<FJsonValue>> Actors;
			if (GEditor)
			{
				if (USelection* Selection = GEditor->GetSelectedActors())
				{
					TArray<AActor*> Selected;
					Selection->GetSelectedObjects<AActor>(Selected);
					for (AActor* Actor : Selected)
					{
						if (IsValid(Actor))
						{
							Actors.Add(MakeShared<FJsonValueObject>(MakeActorObject(Actor)));
						}
					}
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetArrayField(TEXT("actors"), Actors);
			return SuccessJson(Result);
		}

		FString GetActorDetails(const TSharedPtr<FJsonObject>& Args)
		{
			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Actor = FindActorByNameOrLabel(World, Name);
			if (!Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));

			TInlineComponentArray<UActorComponent*> Components(Actor);
			TArray<TSharedPtr<FJsonValue>> ComponentArray;
			for (UActorComponent* Component : Components)
			{
				if (!IsValid(Component))
				{
					continue;
				}

				TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
				ComponentJson->SetStringField(TEXT("name"), Component->GetName());
				ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());

				if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
				{
					ComponentJson->SetObjectField(TEXT("relativeLocation"), MakeVectorObject(SceneComponent->GetRelativeLocation()));
					ComponentJson->SetObjectField(TEXT("relativeRotation"), MakeRotatorObject(SceneComponent->GetRelativeRotation()));
					ComponentJson->SetObjectField(TEXT("relativeScale"), MakeVectorObject(SceneComponent->GetRelativeScale3D()));
					ComponentJson->SetStringField(TEXT("mobility"), MobilityToString(SceneComponent->Mobility.GetValue()));
				}

				if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
				{
					if (const UStaticMesh* Mesh = StaticMeshComponent->GetStaticMesh())
					{
						ComponentJson->SetStringField(TEXT("staticMesh"), Mesh->GetPathName());
					}
					ComponentJson->SetNumberField(TEXT("materialCount"), StaticMeshComponent->GetNumMaterials());
				}

				ComponentArray.Add(MakeShared<FJsonValueObject>(ComponentJson));
			}

			Result->SetNumberField(TEXT("componentCount"), ComponentArray.Num());
			Result->SetArrayField(TEXT("components"), ComponentArray);
			return SuccessJson(Result);
		}

		FString GetContentSummary(const TSharedPtr<FJsonObject>& Args)
		{
			FString SearchRoot = TEXT("/Game");
			Args->TryGetStringField(TEXT("path"), SearchRoot);
			if (SearchRoot.IsEmpty())
			{
				SearchRoot = TEXT("/Game");
			}

			double MaxClassesNumber = 30.0;
			Args->TryGetNumberField(TEXT("maxClasses"), MaxClassesNumber);
			const int32 MaxClasses = FMath::Clamp(static_cast<int32>(MaxClassesNumber), 1, 200);

			int32 TotalAssets = 0;
			TMap<FString, int32> CountsByClass;
			ComputeContentCounts(SearchRoot, TotalAssets, CountsByClass);

			CountsByClass.ValueSort([](const int32& A, const int32& B) { return A > B; });

			TArray<TSharedPtr<FJsonValue>> Classes;
			for (const TPair<FString, int32>& Pair : CountsByClass)
			{
				if (Classes.Num() >= MaxClasses)
				{
					break;
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("class"), Pair.Key);
				Entry->SetNumberField(TEXT("count"), Pair.Value);
				Classes.Add(MakeShared<FJsonValueObject>(Entry));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), SearchRoot);
			Result->SetNumberField(TEXT("totalAssets"), TotalAssets);
			Result->SetNumberField(TEXT("classCount"), CountsByClass.Num());
			Result->SetBoolField(TEXT("truncated"), CountsByClass.Num() > Classes.Num());
			Result->SetArrayField(TEXT("byClass"), Classes);
			return SuccessJson(Result);
		}

		FString GetBootstrapContextJson()
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("purpose"), TEXT("Read this first. It gives an agent a compact, read-only strategy for understanding this Unreal project before taking action."));
			Result->SetStringField(TEXT("projectName"), GetProjectName());
			Result->SetStringField(TEXT("projectId"), FWorldDataMCPServer::GetProjectId());
			Result->SetStringField(TEXT("serverName"), FWorldDataMCPServer::GetServerName());
			Result->SetStringField(TEXT("mcpUrl"), FWorldDataMCPServer::GetMcpUrl());
			Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

			TSharedRef<FJsonObject> Editor = MakeShared<FJsonObject>();
			UWorld* World = GetEditorWorld();
			if (World)
			{
				Editor->SetStringField(TEXT("levelName"), World->GetMapName());
				Editor->SetStringField(TEXT("levelPackage"), World->GetOutermost()->GetName());

				int32 ActorCount = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					++ActorCount;
				}
				Editor->SetNumberField(TEXT("actorCount"), ActorCount);
			}
			Editor->SetBoolField(TEXT("isPlayInEditor"), GEditor ? (GEditor->PlayWorld != nullptr) : false);
			Editor->SetNumberField(TEXT("selectedActorCount"), GEditor ? GEditor->GetSelectedActorCount() : 0);
			Result->SetObjectField(TEXT("editor"), Editor);

			// Compact content histogram so an agent immediately knows what asset types exist.
			{
				int32 TotalAssets = 0;
				TMap<FString, int32> CountsByClass;
				ComputeContentCounts(TEXT("/Game"), TotalAssets, CountsByClass);
				CountsByClass.ValueSort([](const int32& A, const int32& B) { return A > B; });

				TArray<TSharedPtr<FJsonValue>> TopClasses;
				for (const TPair<FString, int32>& Pair : CountsByClass)
				{
					if (TopClasses.Num() >= 10)
					{
						break;
					}
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("class"), Pair.Key);
					Entry->SetNumberField(TEXT("count"), Pair.Value);
					TopClasses.Add(MakeShared<FJsonValueObject>(Entry));
				}

				TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
				Content->SetStringField(TEXT("path"), TEXT("/Game"));
				Content->SetNumberField(TEXT("totalAssets"), TotalAssets);
				Content->SetNumberField(TEXT("classCount"), CountsByClass.Num());
				Content->SetArrayField(TEXT("topClasses"), TopClasses);
				Result->SetObjectField(TEXT("content"), Content);
			}

			TArray<TSharedPtr<FJsonValue>> ReadOrder;
			auto AddStep = [&ReadOrder](const FString& Uri, const FString& Why)
			{
				TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
				Step->SetStringField(TEXT("uri"), Uri);
				Step->SetStringField(TEXT("why"), Why);
				ReadOrder.Add(MakeShared<FJsonValueObject>(Step));
			};

			AddStep(TEXT("worlddata://project/info"), TEXT("Verify project identity and the active MCP endpoint."));
			AddStep(TEXT("worlddata://codex/policy-snapshot"), TEXT("Inspect explicit local Codex approval, sandbox, model, and MCP configuration, if present."));
			AddStep(TEXT("worlddata://editor/selection"), TEXT("See what the user currently has selected to act in context."));
			AddStep(TEXT("worlddata://level/actors"), TEXT("Understand the current editor world before spawning or selecting actors."));
			AddStep(TEXT("worlddata://content/summary"), TEXT("Survey asset types before browsing details or creating duplicates."));
			AddStep(TEXT("worlddata://content/assets"), TEXT("List concrete asset paths when you need exact references."));
			Result->SetArrayField(TEXT("recommendedReadOrder"), ReadOrder);

			TArray<TSharedPtr<FJsonValue>> Notes;
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Resources are read-only and compact by design.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Use tools for mutations only after reading enough context.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Prefer get_actor_details/get_selected_actors for focused inspection over listing all actors.")));
			Result->SetArrayField(TEXT("notes"), Notes);

			return JsonObjectToString(Result);
		}
	}
}
