#include "WorldDataMCPTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPServer.h"
#include "WorldDataMCPSpatialTools.h"
#include "WorldDataSceneBriefStore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "StaticMeshResources.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "JsonObjectConverter.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "UnrealClient.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"
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

	// Mutating tools refuse to run during Play In Editor: editing the editor world while a
	// PIE session is live is ambiguous and can corrupt state. Read tools stay available.
	bool IsPlayInEditorActive()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
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

	// Emit transform fields only when they deviate from the default (location/rotation = 0,
	// scale = 1). Most actors and components sit at a default rotation/scale, so omitting
	// those fields keeps level/actor listings markedly more token-compact for the agent.
	// Convention: an absent field means the default value.
	void AddTransformFields(const TSharedRef<FJsonObject>& Json, const FVector& Location, const FRotator& Rotation, const FVector& Scale,
		const TCHAR* LocationKey, const TCHAR* RotationKey, const TCHAR* ScaleKey)
	{
		if (!Location.IsNearlyZero())
		{
			Json->SetObjectField(LocationKey, MakeVectorObject(Location));
		}
		if (!Rotation.IsNearlyZero())
		{
			Json->SetObjectField(RotationKey, MakeRotatorObject(Rotation));
		}
		if (!Scale.Equals(FVector::OneVector))
		{
			Json->SetObjectField(ScaleKey, MakeVectorObject(Scale));
		}
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

	// Resolve a UClass from a class path/name, an engine class short name, or a Blueprint
	// asset path (returns its generated class). Used for class-reference property assignment.
	UClass* ResolveClassByText(const FString& ClassText)
	{
		if (ClassText.IsEmpty())
		{
			return nullptr;
		}

		UClass* Found = FindObject<UClass>(nullptr, *ClassText);
		if (!Found)
		{
			Found = LoadObject<UClass>(nullptr, *ClassText);
		}
		if (!Found && !ClassText.StartsWith(TEXT("/")))
		{
			Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassText));
		}
		if (!Found && ClassText.StartsWith(TEXT("/")))
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(ClassText));
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				Found = Blueprint->GeneratedClass;
			}
			else
			{
				Found = Cast<UClass>(Object);
			}
		}
		return Found;
	}

	// Apply a JSON value to a single property. Object/class reference properties accept an
	// asset/class path string (e.g. set a StaticMesh or Material by path) or null to clear;
	// everything else (numbers, bools, strings, enums, structs, arrays, maps) falls back to
	// FJsonObjectConverter. Returns false with OutError on failure.
	bool ApplyJsonValueToProperty(const TSharedPtr<FJsonValue>& JsonValue, FProperty* Property, void* ValuePtr, FString& OutError)
	{
		const bool bIsNull = !JsonValue.IsValid() || JsonValue->Type == EJson::Null;

		if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
		{
			if (bIsNull)
			{
				ClassProp->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			const FString Text = JsonValue->AsString();
			UClass* Resolved = ResolveClassByText(Text);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("Could not resolve class: %s"), *Text);
				return false;
			}
			if (ClassProp->MetaClass && !Resolved->IsChildOf(ClassProp->MetaClass))
			{
				OutError = FString::Printf(TEXT("Class '%s' is not a %s."), *Resolved->GetName(), *ClassProp->MetaClass->GetName());
				return false;
			}
			ClassProp->SetObjectPropertyValue(ValuePtr, Resolved);
			return true;
		}

		if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
		{
			if (bIsNull)
			{
				SoftClassProp->SetPropertyValue(ValuePtr, FSoftObjectPtr());
				return true;
			}
			UClass* Resolved = ResolveClassByText(JsonValue->AsString());
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("Could not resolve class: %s"), *JsonValue->AsString());
				return false;
			}
			SoftClassProp->SetPropertyValue(ValuePtr, FSoftObjectPtr(Resolved));
			return true;
		}

		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			if (bIsNull || JsonValue->AsString().IsEmpty())
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			const FString Path = JsonValue->AsString();
			UClass* RequiredClass = ObjProp->PropertyClass ? ObjProp->PropertyClass.Get() : UObject::StaticClass();
			UObject* Loaded = StaticLoadObject(RequiredClass, nullptr, *NormalizeAssetObjectPath(Path));
			if (!Loaded)
			{
				Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(Path));
			}
			if (!Loaded)
			{
				OutError = FString::Printf(TEXT("Asset not found: %s"), *Path);
				return false;
			}
			if (ObjProp->PropertyClass && !Loaded->IsA(ObjProp->PropertyClass))
			{
				OutError = FString::Printf(TEXT("Asset '%s' is not a %s."), *Loaded->GetName(), *ObjProp->PropertyClass->GetName());
				return false;
			}
			ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
			return true;
		}

		if (!FJsonObjectConverter::JsonValueToUProperty(JsonValue, Property, ValuePtr, 0, 0))
		{
			OutError = TEXT("Type mismatch or unsupported property type.");
			return false;
		}
		return true;
	}

	struct FPropertyPathStep
	{
		bool bIsSubscript = false;  // "[...]" — array index or map key
		FString Name;               // member name (when !bIsSubscript)
		FString Key;                // subscript text, quotes stripped (when bIsSubscript)
		int32 Index = 0;            // parsed int (valid when bNumeric)
		bool bNumeric = false;
	};

	// Split "Struct.Member" / "Array[0]" / "Map[\"key\"]" / "Foo[1].Bar" into navigable steps.
	bool TokenizePropertyPath(const FString& Path, TArray<FPropertyPathStep>& OutSteps, FString& OutError)
	{
		const int32 Len = Path.Len();
		int32 i = 0;
		while (i < Len)
		{
			const TCHAR Ch = Path[i];
			if (Ch == TEXT('.'))
			{
				++i;
				continue;
			}
			if (Ch == TEXT('['))
			{
				int32 Close = INDEX_NONE;
				for (int32 j = i + 1; j < Len; ++j) { if (Path[j] == TEXT(']')) { Close = j; break; } }
				if (Close == INDEX_NONE) { OutError = TEXT("Unmatched '[' in property path."); return false; }
				FString Inner = Path.Mid(i + 1, Close - i - 1).TrimStartAndEnd();
				// Strip surrounding quotes for string map keys.
				if (Inner.Len() >= 2
					&& ((Inner.StartsWith(TEXT("\"")) && Inner.EndsWith(TEXT("\"")))
						|| (Inner.StartsWith(TEXT("'")) && Inner.EndsWith(TEXT("'")))))
				{
					Inner = Inner.Mid(1, Inner.Len() - 2);
				}
				if (Inner.IsEmpty()) { OutError = TEXT("Empty subscript in property path."); return false; }
				FPropertyPathStep Step;
				Step.bIsSubscript = true;
				Step.Key = Inner;
				Step.bNumeric = Inner.IsNumeric();
				Step.Index = Step.bNumeric ? FCString::Atoi(*Inner) : 0;
				OutSteps.Add(Step);
				i = Close + 1;
			}
			else
			{
				const int32 Start = i;
				while (i < Len && Path[i] != TEXT('.') && Path[i] != TEXT('[')) { ++i; }
				const FString Seg = Path.Mid(Start, i - Start).TrimStartAndEnd();
				if (Seg.IsEmpty()) { OutError = TEXT("Empty segment in property path."); return false; }
				FPropertyPathStep Step;
				Step.Name = Seg;
				OutSteps.Add(Step);
			}
		}
		if (OutSteps.Num() == 0 || OutSteps[0].bIsSubscript)
		{
			OutError = TEXT("Property path must start with a property name.");
			return false;
		}
		return true;
	}

	// Navigate a (possibly nested) property path to the leaf property + value pointer.
	// OutTopProp is the top-level property on the object (used for the editability check
	// and the change notification, which the editor expects at the top level).
	bool ResolvePropertyPath(UObject* Object, const FString& Path, FProperty*& OutLeafProp, void*& OutLeafPtr, FProperty*& OutTopProp, FString& OutError)
	{
		TArray<FPropertyPathStep> Steps;
		if (!TokenizePropertyPath(Path, Steps, OutError))
		{
			return false;
		}

		FProperty* CurProp = Object->GetClass()->FindPropertyByName(FName(*Steps[0].Name));
		if (!CurProp)
		{
			OutError = FString::Printf(TEXT("Property not found: %s"), *Steps[0].Name);
			return false;
		}
		void* CurPtr = CurProp->ContainerPtrToValuePtr<void>(Object);
		OutTopProp = CurProp;

		for (int32 s = 1; s < Steps.Num(); ++s)
		{
			const FPropertyPathStep& Step = Steps[s];
			if (Step.bIsSubscript)
			{
				if (FArrayProperty* ArrProp = CastField<FArrayProperty>(CurProp))
				{
					if (!Step.bNumeric)
					{
						OutError = FString::Printf(TEXT("Array subscript must be a number, got '%s'."), *Step.Key);
						return false;
					}
					FScriptArrayHelper Helper(ArrProp, CurPtr);
					if (Step.Index < 0 || Step.Index >= Helper.Num())
					{
						OutError = FString::Printf(TEXT("Array index %d out of range (size %d)."), Step.Index, Helper.Num());
						return false;
					}
					CurProp = ArrProp->Inner;
					CurPtr = Helper.GetRawPtr(Step.Index);
				}
				else if (FMapProperty* MapProp = CastField<FMapProperty>(CurProp))
				{
					FScriptMapHelper Helper(MapProp, CurPtr);
					FProperty* KeyProp = MapProp->KeyProp;
					void* TempKey = FMemory::Malloc(FMath::Max<int32>(KeyProp->GetSize(), 1), KeyProp->GetMinAlignment());
					KeyProp->InitializeValue(TempKey);
					const TCHAR* Parsed = KeyProp->ImportText_Direct(*Step.Key, TempKey, nullptr, PPF_None);
					const int32 MapIndex = Parsed ? Helper.FindMapIndexWithKey(TempKey) : INDEX_NONE;
					KeyProp->DestroyValue(TempKey);
					FMemory::Free(TempKey);
					if (!Parsed)
					{
						OutError = FString::Printf(TEXT("Could not parse map key '%s'."), *Step.Key);
						return false;
					}
					if (MapIndex == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("Map key not found: %s"), *Step.Key);
						return false;
					}
					CurProp = MapProp->ValueProp;
					CurPtr = Helper.GetValuePtr(MapIndex);
				}
				else
				{
					OutError = FString::Printf(TEXT("Subscript '[%s]' expects an array or map property (sets are not individually addressable — replace the whole set instead)."), *Step.Key);
					return false;
				}
			}
			else
			{
				FStructProperty* StructProp = CastField<FStructProperty>(CurProp);
				if (!StructProp)
				{
					OutError = FString::Printf(TEXT("Segment '%s' expects a struct property."), *Step.Name);
					return false;
				}
				FProperty* Member = StructProp->Struct->FindPropertyByName(FName(*Step.Name));
				if (!Member)
				{
					OutError = FString::Printf(TEXT("Struct member not found: %s"), *Step.Name);
					return false;
				}
				CurProp = Member;
				CurPtr = Member->ContainerPtrToValuePtr<void>(CurPtr);
			}
		}
		OutLeafProp = CurProp;
		OutLeafPtr = CurPtr;
		return true;
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

	// bIncludeBounds is opt-in: world-space AABB is cheap but adds ~4 numbers per actor, which
	// is noise in a 1000-actor listing yet valuable for a single-actor read or a spatial query.
	TSharedPtr<FJsonObject> MakeActorObject(AActor* Actor, bool bIncludeBounds = false)
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

		AddTransformFields(ActorJson, Actor->GetActorLocation(), Actor->GetActorRotation(), Actor->GetActorScale3D(),
			TEXT("location"), TEXT("rotation"), TEXT("scale"));

		if (const USceneComponent* Root = Actor->GetRootComponent())
		{
			ActorJson->SetStringField(TEXT("mobility"), MobilityToString(Root->Mobility.GetValue()));
		}

		ActorJson->SetNumberField(TEXT("componentCount"), CountActorComponents(Actor));
		ActorJson->SetBoolField(TEXT("selected"), Actor->IsSelected());

		// Attachment parent — emitted only when present, so a flat list stays flat but a child
		// actor's place in the hierarchy is never silently dropped.
		if (const AActor* AttachParent = Actor->GetAttachParentActor())
		{
			ActorJson->SetStringField(TEXT("attachParent"), AttachParent->GetActorLabel());
		}

		if (bIncludeBounds)
		{
			const FBox Bounds = WorldDataMCP::SpatialTools::GetActorWorldBounds(Actor);
			if (Bounds.IsValid)
			{
				ActorJson->SetObjectField(TEXT("bounds"), WorldDataMCP::SpatialTools::BoxToJson(Bounds));
			}
		}

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

			double OffsetNumber = 0.0;
			Args->TryGetNumberField(TEXT("offset"), OffsetNumber);
			const int32 Offset = FMath::Max(0, static_cast<int32>(OffsetNumber));

			bool bIncludeBounds = false;
			Args->TryGetBoolField(TEXT("includeBounds"), bIncludeBounds);

			bool bGroupByFolder = false;
			Args->TryGetBoolField(TEXT("groupByFolder"), bGroupByFolder);

			TArray<TSharedPtr<FJsonValue>> Actors;
			int32 MatchedCount = 0;
			// Folder -> {count, per-class histogram}: a free semantic grouping (the outliner's
			// own structure) that turns a flat list into "what lives where" without clustering.
			TMap<FString, int32> FolderCounts;
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

				if (bGroupByFolder)
				{
					const FName FolderPath = Actor->GetFolderPath();
					FolderCounts.FindOrAdd(FolderPath.IsNone() ? FString(TEXT("(none)")) : FolderPath.ToString())++;
				}

				// 0-based index among matches; emit the window [Offset, Offset+MaxResults).
				const int32 MatchIndex = MatchedCount;
				++MatchedCount;
				if (MatchIndex >= Offset && Actors.Num() < MaxResults)
				{
					Actors.Add(MakeShared<FJsonValueObject>(MakeActorObject(Actor, bIncludeBounds)));
				}
			}

			const int32 NextOffset = Offset + Actors.Num();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Actors.Num());
			Result->SetNumberField(TEXT("matchedCount"), MatchedCount);
			Result->SetNumberField(TEXT("offset"), Offset);
			Result->SetBoolField(TEXT("truncated"), NextOffset < MatchedCount);
			if (NextOffset < MatchedCount)
			{
				Result->SetNumberField(TEXT("nextOffset"), NextOffset);
			}
			Result->SetArrayField(TEXT("actors"), Actors);

			if (bGroupByFolder)
			{
				FolderCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });
				TArray<TSharedPtr<FJsonValue>> Folders;
				for (const TPair<FString, int32>& Pair : FolderCounts)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("folder"), Pair.Key);
					Entry->SetNumberField(TEXT("count"), Pair.Value);
					Folders.Add(MakeShared<FJsonValueObject>(Entry));
				}
				Result->SetArrayField(TEXT("folders"), Folders);
			}
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

			double OffsetNumber = 0.0;
			Args->TryGetNumberField(TEXT("offset"), OffsetNumber);
			const int32 Offset = FMath::Max(0, static_cast<int32>(OffsetNumber));

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

				const int32 MatchIndex = MatchedCount;
				++MatchedCount;
				if (MatchIndex >= Offset && Assets.Num() < MaxResults)
				{
					TSharedRef<FJsonObject> AssetJson = MakeShared<FJsonObject>();
					AssetJson->SetStringField(TEXT("name"), AssetName);
					AssetJson->SetStringField(TEXT("path"), ObjectPath);
					AssetJson->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
					AssetJson->SetStringField(TEXT("class"), ClassName);
					Assets.Add(MakeShared<FJsonValueObject>(AssetJson));
				}
			}

			const int32 NextOffset = Offset + Assets.Num();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("count"), Assets.Num());
			Result->SetNumberField(TEXT("matchedCount"), MatchedCount);
			Result->SetNumberField(TEXT("offset"), Offset);
			Result->SetBoolField(TEXT("truncated"), NextOffset < MatchedCount);
			if (NextOffset < MatchedCount)
			{
				Result->SetNumberField(TEXT("nextOffset"), NextOffset);
			}
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

		FString SetSceneBrief(const TSharedPtr<FJsonObject>& Args)
		{
			::WorldDataMCP::FSceneBrief Brief;
			Args->TryGetStringField(TEXT("oneIdea"), Brief.OneIdea);
			Args->TryGetStringField(TEXT("language"), Brief.Language);
			Args->TryGetStringField(TEXT("hero"), Brief.Hero);
			Args->TryGetStringField(TEXT("palette"), Brief.Palette);
			Args->TryGetStringField(TEXT("atmosphere"), Brief.Atmosphere);
			Args->TryGetStringField(TEXT("boundary"), Brief.Boundary);

			if (UWorld* World = GetEditorWorld())
			{
				Brief.LevelName = World->GetMapName();
			}

			FString Error;
			if (!::WorldDataMCP::SetSceneBrief(Brief, Error))
			{
				return ErrorJson(Error);
			}

			const ::WorldDataMCP::FSceneBrief Stored = ::WorldDataMCP::GetActiveSceneBrief();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("briefActive"), true);
			Result->SetStringField(TEXT("level"), Stored.LevelName);
			Result->SetStringField(TEXT("oneIdea"), Stored.OneIdea);
			Result->SetStringField(TEXT("language"), Stored.Language);
			Result->SetStringField(TEXT("hero"), Stored.Hero);
			Result->SetStringField(TEXT("palette"), Stored.Palette);
			Result->SetStringField(TEXT("atmosphere"), Stored.Atmosphere);
			Result->SetStringField(TEXT("boundary"), Stored.Boundary);
			Result->SetStringField(TEXT("note"), TEXT("Scene generation is now unlocked for this level. Generation tools (place_meshes / lay_meshes_along_spline / add_foliage_instances / spawn_actor with a mesh / apply_native_pcg_rule) will run. Don't skip the mandatory atmosphere pass."));
			return SuccessJson(Result);
		}

		FString GetSceneBrief(const TSharedPtr<FJsonObject>& Args)
		{
			const ::WorldDataMCP::FSceneBrief B = ::WorldDataMCP::GetActiveSceneBrief();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("briefActive"), B.bValid);
			if (B.bValid)
			{
				Result->SetStringField(TEXT("level"), B.LevelName);
				Result->SetStringField(TEXT("oneIdea"), B.OneIdea);
				Result->SetStringField(TEXT("language"), B.Language);
				Result->SetStringField(TEXT("hero"), B.Hero);
				Result->SetStringField(TEXT("palette"), B.Palette);
				Result->SetStringField(TEXT("atmosphere"), B.Atmosphere);
				Result->SetStringField(TEXT("boundary"), B.Boundary);
			}
			else
			{
				Result->SetStringField(TEXT("note"), TEXT("No active scene brief. Call set_scene_brief before generating."));
			}
			return SuccessJson(Result);
		}

		FString ClearSceneBrief(const TSharedPtr<FJsonObject>& Args)
		{
			::WorldDataMCP::ClearSceneBrief();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("briefActive"), false);
			Result->SetStringField(TEXT("note"), TEXT("Scene brief cleared; scene generation is gated again until set_scene_brief is called."));
			return SuccessJson(Result);
		}

		FString SpawnActor(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("spawn_actor is disabled while Play In Editor is active. Stop PIE first."));
			}

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

			// Scene-generation gate: spawning a static mesh is content placement, so it needs an
			// active scene brief. Infra spawns (by class: lights, volumes, splines) are exempt.
			if (!StaticMeshPath.IsEmpty())
			{
				FString GateReason;
				if (!::WorldDataMCP::HasActiveSceneBrief(World->GetMapName(), GateReason))
				{
					return ErrorJson(GateReason);
				}
			}

			UStaticMesh* Mesh = nullptr;
			UClass* ActorClass = nullptr;
			FString ClassText = TEXT("Actor");
			AActor* Actor = nullptr;
			if (!StaticMeshPath.IsEmpty())
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, *NormalizeAssetObjectPath(StaticMeshPath));
				if (!Mesh)
				{
					return ErrorJson(FString::Printf(TEXT("Static mesh not found: %s"), *StaticMeshPath));
				}
			}
			else
			{
				Args->TryGetStringField(TEXT("class"), ClassText);
				ActorClass = ResolveActorClass(ClassText);
				if (!ActorClass)
				{
					return ErrorJson(FString::Printf(TEXT("Actor class not found or not spawnable: %s"), *ClassText));
				}
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "SpawnActor", "MCP Spawn Actor"));
			World->Modify();

			if (Mesh)
			{
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
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor, /*bIncludeBounds=*/true));

			// Attachment children — the other half of the hierarchy (attachParent is on the actor).
			TArray<AActor*> AttachedActors;
			Actor->GetAttachedActors(AttachedActors);
			if (AttachedActors.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Children;
				for (const AActor* Child : AttachedActors)
				{
					if (IsValid(Child))
					{
						Children.Add(MakeShared<FJsonValueString>(Child->GetActorLabel()));
					}
				}
				Result->SetArrayField(TEXT("attachChildren"), Children);
			}

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
					AddTransformFields(ComponentJson, SceneComponent->GetRelativeLocation(), SceneComponent->GetRelativeRotation(), SceneComponent->GetRelativeScale3D(),
						TEXT("relativeLocation"), TEXT("relativeRotation"), TEXT("relativeScale"));
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

		FString AnalyzeScenePerformance(const TSharedPtr<FJsonObject>& Args)
		{
			UWorld* World = GetEditorWorld();
			if (!World)
			{
				return ErrorJson(TEXT("Editor world is not available."));
			}

			double TopActorsNumber = 10.0;
			Args->TryGetNumberField(TEXT("topActors"), TopActorsNumber);
			const int32 TopActors = FMath::Clamp(static_cast<int32>(TopActorsNumber), 0, 50);

			// An actor is flagged "heavy" above this LOD0 triangle budget. Tunable per call so an
			// agent can sweep a dense scene at a higher bar without re-reading every actor.
			double HeavyTriangleThresholdNumber = 500000.0;
			Args->TryGetNumberField(TEXT("heavyTriangleThreshold"), HeavyTriangleThresholdNumber);
			const int64 HeavyTriangleThreshold = FMath::Max<int64>(1, static_cast<int64>(HeavyTriangleThresholdNumber));

			// Per-mesh LOD0 cost, computed once and reused — a scene reuses the same meshes heavily,
			// so caching avoids touching render data thousands of times.
			struct FMeshCost
			{
				int64 Triangles = 0;
				int32 Sections = 0;
				int32 NumLODs = 0;
				bool bNanite = false;
			};
			TMap<const UStaticMesh*, FMeshCost> MeshCostCache;
			auto GetMeshCost = [&MeshCostCache](const UStaticMesh* Mesh) -> FMeshCost
			{
				if (const FMeshCost* Cached = MeshCostCache.Find(Mesh))
				{
					return *Cached;
				}
				FMeshCost Cost;
				if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
				{
					Cost.NumLODs = RenderData->LODResources.Num();
					if (RenderData->LODResources.Num() > 0)
					{
						const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
						Cost.Triangles = LOD0.GetNumTriangles();
						Cost.Sections = LOD0.Sections.Num();
					}
				}
#if WITH_EDITORONLY_DATA
				Cost.bNanite = Mesh->NaniteSettings.bEnabled;
#endif
				MeshCostCache.Add(Mesh, Cost);
				return Cost;
			};

			int32 ActorCount = 0;
			int32 ComponentCount = 0;
			int32 StaticMeshComponentCount = 0;
			int32 InstancedComponentCount = 0;
			int64 InstanceCount = 0;
			int32 SkeletalMeshComponentCount = 0;
			int64 TotalTriangles = 0;
			int64 EstimatedDrawCalls = 0;
			int32 NaniteMeshCount = 0;
			int32 MeshesMissingLODs = 0;

			int32 LightTotal = 0;
			int32 LightStatic = 0;
			int32 LightStationary = 0;
			int32 LightMovable = 0;
			int32 ShadowCastingMovableLights = 0;

			TSet<const UStaticMesh*> UniqueMeshes;
			TSet<const UMaterialInterface*> UniqueMaterials;
			int32 TranslucentMaterials = 0;

			// Heaviest actors, tracked by LOD0 triangle total so an agent can jump straight to the
			// worst offenders instead of paging every actor.
			struct FActorCost
			{
				AActor* Actor = nullptr;
				int64 Triangles = 0;
				int32 Sections = 0;
				int32 StaticMeshComponents = 0;
			};
			TArray<FActorCost> ActorCosts;

			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor))
				{
					continue;
				}
				++ActorCount;

				FActorCost ActorCost;
				ActorCost.Actor = Actor;

				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				for (UActorComponent* Component : Components)
				{
					if (!IsValid(Component))
					{
						continue;
					}
					++ComponentCount;

					if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component))
					{
						UStaticMesh* Mesh = SMC->GetStaticMesh();
						if (!Mesh)
						{
							continue;
						}
						++StaticMeshComponentCount;

						bool bNewMesh = false;
						UniqueMeshes.Add(Mesh, &bNewMesh);
						const FMeshCost Cost = GetMeshCost(Mesh);
						if (bNewMesh)
						{
							if (Cost.bNanite)
							{
								++NaniteMeshCount;
							}
							if (Cost.NumLODs <= 1)
							{
								++MeshesMissingLODs;
							}
						}

						// Instanced meshes draw all instances; GPU instancing keeps the draw-call
						// cost at one set of sections regardless of instance count.
						int64 Multiplier = 1;
						if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Component))
						{
							++InstancedComponentCount;
							Multiplier = FMath::Max(0, ISM->GetInstanceCount());
							InstanceCount += Multiplier;
						}

						const int64 Tris = Cost.Triangles * Multiplier;
						TotalTriangles += Tris;
						// Nanite collapses sections into a single virtualized pass; don't charge it
						// per-section draw calls.
						EstimatedDrawCalls += Cost.bNanite ? 1 : Cost.Sections;

						ActorCost.Triangles += Tris;
						ActorCost.Sections += Cost.bNanite ? 1 : Cost.Sections;
						++ActorCost.StaticMeshComponents;

						TArray<UMaterialInterface*> UsedMaterials;
						SMC->GetUsedMaterials(UsedMaterials);
						for (const UMaterialInterface* Material : UsedMaterials)
						{
							if (!Material)
							{
								continue;
							}
							bool bNewMaterial = false;
							UniqueMaterials.Add(Material, &bNewMaterial);
							if (bNewMaterial && Material->GetBlendMode() != BLEND_Opaque && Material->GetBlendMode() != BLEND_Masked)
							{
								++TranslucentMaterials;
							}
						}
					}
					else if (USkeletalMeshComponent* SkelMC = Cast<USkeletalMeshComponent>(Component))
					{
						if (SkelMC->GetSkeletalMeshAsset())
						{
							++SkeletalMeshComponentCount;
						}
					}
					else if (ULightComponent* Light = Cast<ULightComponent>(Component))
					{
						if (!Light->GetVisibleFlag())
						{
							continue;
						}
						++LightTotal;
						switch (Light->Mobility)
						{
						case EComponentMobility::Static: ++LightStatic; break;
						case EComponentMobility::Stationary: ++LightStationary; break;
						case EComponentMobility::Movable:
							++LightMovable;
							if (Light->CastShadows)
							{
								++ShadowCastingMovableLights;
							}
							break;
						default: break;
						}
					}
				}

				if (ActorCost.Triangles > 0)
				{
					ActorCosts.Add(ActorCost);
				}
			}

			TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetNumberField(TEXT("actorCount"), ActorCount);
			Summary->SetNumberField(TEXT("componentCount"), ComponentCount);
			Summary->SetNumberField(TEXT("staticMeshComponents"), StaticMeshComponentCount);
			Summary->SetNumberField(TEXT("instancedComponents"), InstancedComponentCount);
			Summary->SetNumberField(TEXT("instanceCount"), static_cast<double>(InstanceCount));
			Summary->SetNumberField(TEXT("skeletalMeshComponents"), SkeletalMeshComponentCount);
			Summary->SetNumberField(TEXT("uniqueStaticMeshes"), UniqueMeshes.Num());
			Summary->SetNumberField(TEXT("totalTriangles"), static_cast<double>(TotalTriangles));
			Summary->SetNumberField(TEXT("estimatedDrawCalls"), static_cast<double>(EstimatedDrawCalls));
			Summary->SetNumberField(TEXT("naniteMeshCount"), NaniteMeshCount);
			Summary->SetNumberField(TEXT("uniqueMaterials"), UniqueMaterials.Num());
			Summary->SetNumberField(TEXT("translucentMaterials"), TranslucentMaterials);

			TSharedRef<FJsonObject> Lights = MakeShared<FJsonObject>();
			Lights->SetNumberField(TEXT("total"), LightTotal);
			Lights->SetNumberField(TEXT("static"), LightStatic);
			Lights->SetNumberField(TEXT("stationary"), LightStationary);
			Lights->SetNumberField(TEXT("movable"), LightMovable);
			Lights->SetNumberField(TEXT("shadowCastingMovable"), ShadowCastingMovableLights);
			Summary->SetObjectField(TEXT("lights"), Lights);

			// Heaviest actors descending by triangle count.
			ActorCosts.Sort([](const FActorCost& A, const FActorCost& B) { return A.Triangles > B.Triangles; });
			TArray<TSharedPtr<FJsonValue>> HeaviestActors;
			for (const FActorCost& Cost : ActorCosts)
			{
				if (HeaviestActors.Num() >= TopActors)
				{
					break;
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Cost.Actor->GetName());
				Entry->SetStringField(TEXT("label"), Cost.Actor->GetActorLabel());
				Entry->SetStringField(TEXT("class"), Cost.Actor->GetClass()->GetName());
				Entry->SetNumberField(TEXT("triangles"), static_cast<double>(Cost.Triangles));
				Entry->SetNumberField(TEXT("sections"), Cost.Sections);
				Entry->SetNumberField(TEXT("staticMeshComponents"), Cost.StaticMeshComponents);
				HeaviestActors.Add(MakeShared<FJsonValueObject>(Entry));
			}

			// Heuristic warnings — cheap, opinionated hints an agent can act on without re-deriving
			// the thresholds. Order roughly by typical render-cost impact.
			TArray<TSharedPtr<FJsonValue>> Warnings;
			auto AddWarning = [&Warnings](const FString& Message)
			{
				Warnings.Add(MakeShared<FJsonValueString>(Message));
			};
			if (ShadowCastingMovableLights > 4)
			{
				AddWarning(FString::Printf(TEXT("%d movable shadow-casting lights — each adds a full dynamic shadow pass; prefer Stationary/Static where possible."), ShadowCastingMovableLights));
			}
			if (EstimatedDrawCalls > 5000)
			{
				AddWarning(FString::Printf(TEXT("~%lld estimated draw calls — consider merging meshes, instancing (ISM/HISM), or enabling Nanite."), EstimatedDrawCalls));
			}
			if (TranslucentMaterials > 0 && TranslucentMaterials * 4 > FMath::Max(1, UniqueMaterials.Num()))
			{
				AddWarning(FString::Printf(TEXT("%d of %d unique materials are translucent/additive — translucency overdraw is expensive on dense scenes."), TranslucentMaterials, UniqueMaterials.Num()));
			}
			if (MeshesMissingLODs > 0 && NaniteMeshCount < UniqueMeshes.Num())
			{
				AddWarning(FString::Printf(TEXT("%d unique static meshes have a single LOD and are not Nanite — distant draws pay full triangle cost."), MeshesMissingLODs));
			}
			for (const FActorCost& Cost : ActorCosts)
			{
				if (Cost.Triangles >= HeavyTriangleThreshold)
				{
					AddWarning(FString::Printf(TEXT("Actor '%s' draws %lld LOD0 triangles (>= %lld threshold)."), *Cost.Actor->GetActorLabel(), Cost.Triangles, HeavyTriangleThreshold));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("level"), World->GetMapName());
			Result->SetBoolField(TEXT("isPlayInEditor"), GEditor ? (GEditor->PlayWorld != nullptr) : false);
			Result->SetObjectField(TEXT("summary"), Summary);
			Result->SetArrayField(TEXT("heaviestActors"), HeaviestActors);
			Result->SetArrayField(TEXT("warnings"), Warnings);
			Result->SetStringField(TEXT("note"), TEXT("Static editor-world estimate from LOD0 render data; not a GPU profile. Triangle/draw-call figures are approximate and ignore culling, HLODs, and runtime spawns."));
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

			AddStep(TEXT("worlddata://context/current-task"), TEXT("Start with the task-oriented aggregate: current level, selection, dirty packages, recent log lines, and recommended next reads."));
			AddStep(TEXT("worlddata://project/info"), TEXT("Verify project identity and the active MCP endpoint."));
			AddStep(TEXT("worlddata://codex/policy-snapshot"), TEXT("Inspect explicit local Codex approval, sandbox, model, and MCP configuration, if present."));
			AddStep(TEXT("worlddata://editor/selection"), TEXT("See what the user currently has selected to act in context."));
			AddStep(TEXT("worlddata://level/actors"), TEXT("Understand the current editor world before spawning or selecting actors."));
			AddStep(TEXT("worlddata://content/summary"), TEXT("Survey asset types before browsing details or creating duplicates."));
			AddStep(TEXT("worlddata://content/assets"), TEXT("List concrete asset paths when you need exact references."));
			Result->SetArrayField(TEXT("recommendedReadOrder"), ReadOrder);

			// Working method: a compact playbook so any connected agent (even a small/local one)
			// works the right way instead of guessing among hundreds of tools.
			TArray<TSharedPtr<FJsonValue>> Method;
			Method.Add(MakeShared<FJsonValueString>(TEXT("1. RESEARCH first — you likely do not natively know UE5's exact levers. Use ue_web_search/ue_fetch_doc for docs, pcg_assist for PCG (returns a node-by-node plan), llm_think to reason. The correct lever is often not the obvious one (sunset = rotate the Atmosphere Sun Light to a low angle, NOT set sky colour).")));
			Method.Add(MakeShared<FJsonValueString>(TEXT("2. PERCEIVE — build a world model before acting. describe_scene gives the spatial digest (bounds, category histogram, dense regions, landmarks + lighting); capture_scene_map renders a top-down map you can see; then drill in with query_actors_in_region / find_nearest_actors / raycast / what_is_under_camera. Never act blind.")));
			Method.Add(MakeShared<FJsonValueString>(TEXT("3. ACT with the most HIGH-LEVEL tool that fits — one intent tool beats orchestrating dozens of low-level verbs.")));
			Method.Add(MakeShared<FJsonValueString>(TEXT("4. VERIFY — capture_viewport and compare to the goal, then iterate. This perceive->act->perceive loop matters most for visual tasks.")));
			Result->SetArrayField(TEXT("workingMethod"), Method);

			// Task -> recommended tools cheat-sheet: steer attention to the few right tools.
			TSharedRef<FJsonObject> CheatSheet = MakeShared<FJsonObject>();
			auto AddTask = [&CheatSheet](const TCHAR* Task, const TCHAR* Tools)
			{
				CheatSheet->SetStringField(Task, Tools);
			};
			AddTask(TEXT("research how to do something"), TEXT("ue_web_search -> ue_fetch_doc; for PCG use pcg_assist; reason with llm_think"));
			AddTask(TEXT("understand the scene / where things are"), TEXT("describe_scene (spatial digest) + capture_scene_map (top-down image) first; then query_actors_in_region / find_nearest_actors / raycast / what_is_under_camera; list_level_actors(groupByFolder,includeBounds) / get_actor_details for specifics"));
			AddTask(TEXT("lighting / time-of-day / sunset"), TEXT("describe_scene first, then set_light_properties on the Atmosphere Sun Light (low pitch), set_fog/set_sky_atmosphere/set_post_process; verify with capture_viewport"));
			AddTask(TEXT("build / populate a scene"), TEXT("read the 'sceneBuilding' section of this bootstrap: a decision tree + per-task tool sequences tying PCG, spline tiling, and placement together. Tile along a path = lay_meshes_along_spline; scatter by density = PCG; place by judgment = place_meshes."));
			AddTask(TEXT("procedural scatter / PCG"), TEXT("pcg_assist(task) for a node plan; create_pcg_graph/add_pcg_node/connect_pcg_nodes; set_pcg_node_settings (dotted paths e.g. SamplerParams.DistanceIncrement); set_static_mesh_spawner_meshes; set_pcg_component_graph to bind a graph to an actor; regenerate_pcg. Check returned failed[]."));
			AddTask(TEXT("lay meshes seamlessly along a spline (road/bridge/fence)"), TEXT("lay_meshes_along_spline {splineActor, mesh, forwardAxis, startMesh?, endMesh?} - serial edge-to-edge, gapless on curves; auto-measures real deck length. Not PCG, survives regenerate_pcg."));
			AddTask(TEXT("place / edit actors"), TEXT("place_meshes (batch spawn + snap-to-ground) is the main placement tool; pick spots with sample_surface_grid/raycast/test_placement_clearance; also spawn_actor, set_actor_transform, duplicate_actor, set_actor_folder, set_object_property."));
			AddTask(TEXT("create assets"), TEXT("create_blueprint/create_material/create_material_instance/create_niagara_system/create_data_table/create_widget_blueprint, then save_asset"));
			AddTask(TEXT("blueprint logic"), TEXT("add_blueprint_variable, add_blueprint_node, connect_blueprint_pins, compile_blueprint"));
			AddTask(TEXT("AI / animation"), TEXT("create_behavior_tree/create_blackboard/add_blackboard_key, create_state_tree/add_state_tree_state, create_anim_blueprint/add_anim_bp_state_machine"));
			AddTask(TEXT("performance check"), TEXT("analyze_scene_performance, then execute_console_command('stat unit')"));
			Result->SetObjectField(TEXT("taskCheatSheet"), CheatSheet);

			TArray<TSharedPtr<FJsonValue>> Notes;
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Resources are read-only and compact by design.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Use tools for mutations only after reading enough context.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Use get_relevant_context(query) with the user's task terms before broad actor, asset, or source scans.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Prefer get_actor_details/get_selected_actors for focused inspection over listing all actors.")));
			Notes.Add(MakeShared<FJsonValueString>(TEXT("Transform fields (location/rotation/scale) are omitted when at their default (location/rotation 0, scale 1); a missing field means the default value.")));
			Result->SetArrayField(TEXT("notes"), Notes);

			// Unified scene-building playbook: ties the procedural (PCG), seamless-spline-tiling, and
			// direct-placement tools into one decision tree so the agent picks the right approach instead
			// of defaulting to whatever it tried last (the cause of the bridge-deck thrash).
			if (TSharedPtr<FJsonObject> SceneBuilding = ParseJsonObject(TEXT(R"JSON(
{
  "purpose": "Pick the approach that matches the task, then follow its tool sequence. The failure mode to avoid is hand-doing precise geometry the wrong way (see gotchas).",
  "loop": "set_scene_brief -> PERCEIVE (describe_scene + capture_scene_map) -> DECIDE (briefRouting from apply_native_pcg_rule or bootstrap briefRouting) -> ACT -> VERIFY (capture_viewport; check returned counts/failed).",
  "briefRouting": {
    "formal": "place_along_axis for allées/hedges/colonnades — avoid packing_scatter. apply_native_pcg_rule ruleId:auto will refuse scatter.",
    "informal": "apply_native_pcg_rule ruleId:auto picks forest_path | packing_scatter | planting_stack from oneIdea keywords. planting_stack = canopy->understory->shrub->groundcover in one call."
  },
  "decisionTree": [
    {"task":"Formal garden / allée / axis-aligned rhythm (language:formal brief)","use":"place_along_axis {mesh, origin, direction, slotCount, spacing, pairedRows?, heroMesh?} — NOT random scatter"},
    {"task":"Layered vegetation stack (canopy + understory + shrub + groundcover)","use":"apply_native_pcg_rule {ruleId:planting_stack, actor, generate:true}"},
    {"task":"Scatter many items by DENSITY over a surface (vegetation, rocks, debris) - distribution matters, exact spots don't","use":"PCG SurfaceSampler scatter (live, non-destructive, parametric)"},
    {"task":"Lay RIGID modular pieces end-to-end along a path with NO gaps (road/bridge/fence), pieces stay rigid","use":"lay_meshes_along_spline (serial edge-to-edge, seamless on curves, measures real deck length, optional start/end caps)"},
    {"task":"A strip must BEND continuously with the curve (path ribbon, ivy, cable)","use":"SplineMesh per-segment deform (reuse BP_SplineMeshes, swap MainMesh)"},
    {"task":"Place individual or a few props by JUDGMENT, resting on the ground","use":"place_meshes (batch spawn + snap-to-surface + optional align-to-normal, one undo)"},
    {"task":"A re-generatable procedural layer with parameters / exclusion zones","use":"PCG graph on a PCGComponent: set_pcg_component_graph then regenerate_pcg"}
  ],
  "workflows": {
    "tile road or bridge along a spline": ["analyze_static_mesh(mesh): travel axis; note bbox may exceed the visible deck","create_spline_actor (or reuse an existing spline actor)","lay_meshes_along_spline {splineActor, mesh, forwardAxis, startMesh?, endMesh?}","focus_actor + capture_viewport to verify"],
    "scatter props or foliage on terrain": ["sample_surface_grid to learn the terrain","pcg_assist('scatter X, exclude roads') for a node plan","create_pcg_graph + add_pcg_node(SurfaceSampler->TransformPoints->StaticMeshSpawner) + set_static_mesh_spawner_meshes","set_pcg_component_graph to bind, regenerate_pcg, capture_viewport"],
    "place individual props by hand": ["describe_scene + capture_scene_map to orient","raycast / sample_surface_grid / find_nearest_actors to choose spots","test_placement_clearance to confirm a spot is free","place_meshes (snapToSurface=true) -> capture_viewport"],
    "formal axis / allée / hedge row": ["set_scene_brief with language:formal","place_along_axis {mesh, origin, direction, slotCount, spacing, pairedRows?, heroMesh?}","capture_viewport to verify rhythm"],
    "layered planting stack": ["set_scene_brief with language:informal","apply_native_pcg_rule {ruleId:planting_stack, actor, generate:true}","enumerate_pcg_instances or list_placements -> capture_viewport"],
    "bind a PCG graph to a spline or actor": ["add_component class=/Script/PCG.PCGComponent (if none exists)","set_pcg_component_graph {actor, graphPath} (NOT set_object_property)","regenerate_pcg"]
  },
  "gotchas": [
    "Bounding box != visible surface: a deck mesh's bbox can exceed its walkable length and leave a see-through gap. lay_meshes_along_spline measures from vertices by default; for PCG, set SamplerParams.DistanceIncrement to the real deck length.",
    "PCG/ISM instances have NO collision: raycast and place_meshes snap onto terrain + collidable meshes, not onto PCG output; measure deck length from mesh verts, not line traces.",
    "PCG's parallel sampler cannot place 'next piece's face on the previous piece's face' (a serial dependency). For seamless rigid tiling use lay_meshes_along_spline, not a StaticMeshSpawner.",
    "set_object_property cannot bind a PCG graph (it hits only the deprecated transient field). Use set_pcg_component_graph.",
    "Check returned failed/applied/placed/meshEntries; success with an empty payload means it did NOT take. Restore the viewport camera after a framed screenshot. Confirm the current level before acting."
  ]
}
)JSON")))
			{
				Result->SetObjectField(TEXT("sceneBuilding"), SceneBuilding);
			}

			return JsonObjectToString(Result);
		}

		// Resolve the target UObject for the reflection tools: an actor by name/label, or
		// (when "component" is supplied) one of its named components.
		static UObject* ResolveTargetObject(const TSharedPtr<FJsonObject>& Args, FString& OutError)
		{
			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				OutError = TEXT("Missing required field 'name'.");
				return nullptr;
			}

			AActor* Actor = FindActorByNameOrLabel(GetEditorWorld(), Name);
			if (!Actor)
			{
				OutError = FString::Printf(TEXT("Actor not found: %s"), *Name);
				return nullptr;
			}

			FString ComponentName;
			if (Args->TryGetStringField(TEXT("component"), ComponentName) && !ComponentName.IsEmpty())
			{
				TInlineComponentArray<UActorComponent*> Components(Actor);
				// Prefer an exact instance-name match (e.g. "StaticMeshComponent0").
				for (UActorComponent* Component : Components)
				{
					if (IsValid(Component) && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
					{
						return Component;
					}
				}
				// Fall back to a class-name match so callers can say "StaticMeshComponent"
				// without knowing the auto-generated instance name.
				for (UActorComponent* Component : Components)
				{
					if (IsValid(Component) && Component->GetClass()->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
					{
						return Component;
					}
				}
				OutError = FString::Printf(TEXT("Component not found on actor '%s': %s"), *Name, *ComponentName);
				return nullptr;
			}

			return Actor;
		}

		FString GetObjectProperties(const TSharedPtr<FJsonObject>& Args)
		{
			FString Error;
			UObject* Object = ResolveTargetObject(Args, Error);
			if (!Object)
			{
				return ErrorJson(Error);
			}

			TArray<FString> WantNames;
			const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
			if (Args->TryGetArrayField(TEXT("properties"), NameValues) && NameValues)
			{
				for (const TSharedPtr<FJsonValue>& Value : *NameValues)
				{
					if (Value.IsValid())
					{
						WantNames.Add(Value->AsString());
					}
				}
			}

			double MaxNumber = 100.0;
			Args->TryGetNumberField(TEXT("maxProperties"), MaxNumber);
			const int32 MaxProps = FMath::Clamp(static_cast<int32>(MaxNumber), 1, 500);

			bool bIncludeDefaults = false;
			Args->TryGetBoolField(TEXT("includeDefaults"), bIncludeDefaults);

			double MaxBytesNumber = 16384.0;
			Args->TryGetNumberField(TEXT("maxBytes"), MaxBytesNumber);
			const int32 MaxBytes = FMath::Clamp(static_cast<int32>(MaxBytesNumber), 512, 1024 * 1024);

			// Compare against the archetype (template/CDO) so, by default, only properties that
			// differ from their default are emitted — that is what the agent actually cares about
			// and it keeps the payload compact. Skipped when specific names are requested.
			const bool bFilterDefaults = !bIncludeDefaults && WantNames.Num() == 0;
			UObject* Baseline = Object->GetArchetype();

			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			int32 Emitted = 0;
			int32 Matched = 0;
			int32 UsedBytes = 0;
			bool bTruncatedBySize = false;
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				const FString PropName = Property->GetName();

				if (WantNames.Num() > 0)
				{
					if (!WantNames.ContainsByPredicate([&PropName](const FString& N) { return N.Equals(PropName, ESearchCase::IgnoreCase); }))
					{
						continue;
					}
				}
				else if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated))
				{
					continue;
				}

				if (bFilterDefaults && Baseline && Property->Identical_InContainer(Object, Baseline))
				{
					continue;
				}

				++Matched;
				if (Emitted >= MaxProps)
				{
					continue;
				}

				TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(Property, Property->ContainerPtrToValuePtr<void>(Object), 0, 0);
				if (!Value.IsValid())
				{
					continue;
				}

				// Bound the total payload: skip a value that would blow the byte budget rather
				// than emit an unbounded blob for a deep struct / large array property.
				TSharedRef<FJsonObject> Measure = MakeShared<FJsonObject>();
				Measure->SetField(TEXT("v"), Value);
				const int32 ValueBytes = JsonObjectToString(Measure).Len();
				if (UsedBytes + ValueBytes > MaxBytes)
				{
					bTruncatedBySize = true;
					continue;
				}
				UsedBytes += ValueBytes;

				Props->SetField(PropName, Value);
				++Emitted;
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("object"), Object->GetName());
			Result->SetStringField(TEXT("class"), Object->GetClass()->GetName());
			Result->SetNumberField(TEXT("propertyCount"), Emitted);
			Result->SetNumberField(TEXT("matchedCount"), Matched);
			Result->SetBoolField(TEXT("truncated"), Matched > Emitted || bTruncatedBySize);
			Result->SetBoolField(TEXT("truncatedBySize"), bTruncatedBySize);
			Result->SetBoolField(TEXT("omitsDefaults"), bFilterDefaults);
			Result->SetObjectField(TEXT("properties"), Props);
			return SuccessJson(Result);
		}

		FString SetObjectProperty(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("set_object_property is disabled while Play In Editor is active. Stop PIE first."));
			}

			FString Error;
			UObject* Object = ResolveTargetObject(Args, Error);
			if (!Object)
			{
				return ErrorJson(Error);
			}

			FString PropName;
			if (!Args->TryGetStringField(TEXT("property"), PropName))
			{
				return ErrorJson(TEXT("Missing required field 'property'."));
			}

			const TSharedPtr<FJsonValue> NewValue = Args->TryGetField(TEXT("value"));
			if (!NewValue.IsValid())
			{
				return ErrorJson(TEXT("Missing required field 'value'."));
			}

			// 'property' may be a path: "RelativeLocation.X", "OverrideMaterials[0]", "Foo[1].Bar".
			FProperty* LeafProp = nullptr;
			void* LeafPtr = nullptr;
			FProperty* TopProp = nullptr;
			FString PathError;
			if (!ResolvePropertyPath(Object, PropName, LeafProp, LeafPtr, TopProp, PathError))
			{
				return ErrorJson(FString::Printf(TEXT("Property path '%s' on %s: %s"), *PropName, *Object->GetClass()->GetName(), *PathError));
			}

			// Safety: editability is governed by the top-level (editor-exposed) property, unless
			// the caller passes force=true. Prevents corrupting read-only / transient /
			// instance-locked properties via reflection.
			bool bForce = false;
			Args->TryGetBoolField(TEXT("force"), bForce);
			if (!bForce)
			{
				const bool bInstance = !Object->HasAnyFlags(RF_ClassDefaultObject);
				const bool bEditable = TopProp->HasAnyPropertyFlags(CPF_Edit) && !TopProp->HasAnyPropertyFlags(CPF_EditConst);
				if (!bEditable || (bInstance && TopProp->HasAnyPropertyFlags(CPF_DisableEditOnInstance)))
				{
					return ErrorJson(FString::Printf(TEXT("Property '%s' is not editable here (read-only/transient/instance-locked). Pass force=true to override."), *TopProp->GetName()));
				}
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "SetObjectProperty", "MCP Set Object Property"));
			Object->Modify();
			Object->PreEditChange(TopProp);

			FString ApplyError;
			const bool bSet = ApplyJsonValueToProperty(NewValue, LeafProp, LeafPtr, ApplyError);
			if (!bSet)
			{
				Transaction.Cancel();
				return ErrorJson(FString::Printf(TEXT("Failed to set property '%s': %s"), *PropName, *ApplyError));
			}

			FPropertyChangedEvent ChangedEvent(TopProp);
			Object->PostEditChangeProperty(ChangedEvent);
			Object->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("object"), Object->GetName());
			Result->SetStringField(TEXT("property"), PropName);
			TSharedPtr<FJsonValue> Updated = FJsonObjectConverter::UPropertyToJsonValue(LeafProp, LeafPtr, 0, 0);
			if (Updated.IsValid())
			{
				Result->SetField(TEXT("value"), Updated);
			}
			return SuccessJson(Result);
		}

		FString SetActorTransform(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("set_actor_transform is disabled while Play In Editor is active. Stop PIE first."));
			}

			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			AActor* Actor = FindActorByNameOrLabel(GetEditorWorld(), Name);
			if (!Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			// Seed with current transform so omitted fields stay unchanged (partial update).
			FVector Location = Actor->GetActorLocation();
			FRotator Rotation = Actor->GetActorRotation();
			FVector Scale = Actor->GetActorScale3D();
			const bool bLoc = TryGetVectorField(Args, TEXT("location"), Location);
			const bool bRot = TryGetRotatorField(Args, TEXT("rotation"), Rotation);
			const bool bScale = TryGetVectorField(Args, TEXT("scale"), Scale);
			if (!bLoc && !bRot && !bScale)
			{
				return ErrorJson(TEXT("Provide at least one of 'location', 'rotation', or 'scale'."));
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "SetActorTransform", "MCP Set Actor Transform"));
			Actor->Modify();
			Actor->SetActorLocationAndRotation(Location, Rotation);
			Actor->SetActorScale3D(Scale);
			Actor->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString DeleteActor(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("delete_actor is disabled while Play In Editor is active. Stop PIE first."));
			}

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

			const FString ActorName = Actor->GetName();
			const FString ActorLabel = Actor->GetActorLabel();

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "DeleteActor", "MCP Delete Actor"));
			if (GEditor)
			{
				GEditor->SelectActor(Actor, false, true);
			}
			World->Modify();
			const bool bDestroyed = World->EditorDestroyActor(Actor, true);
			if (!bDestroyed)
			{
				Transaction.Cancel();
				return ErrorJson(FString::Printf(TEXT("Failed to delete actor: %s"), *Name));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("deletedName"), ActorName);
			Result->SetStringField(TEXT("deletedLabel"), ActorLabel);
			return SuccessJson(Result);
		}

		FString CaptureViewport(const TSharedPtr<FJsonObject>& Args)
		{
			if (!GEditor)
			{
				return ErrorJson(TEXT("Editor is not available."));
			}

			FViewport* Viewport = GEditor->GetActiveViewport();
			if (!Viewport)
			{
				return ErrorJson(TEXT("No active editor viewport."));
			}

			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
			{
				return ErrorJson(TEXT("Active viewport has no valid size."));
			}

			// Force a fresh frame before reading pixels. When the editor window is unfocused,
			// Slate throttles viewport redraws, so ReadPixels would otherwise return a stale
			// (frozen) frame — the agent repeatedly captured byte-identical screenshots and
			// could not visually verify its changes. An explicit Invalidate + Draw bypasses the
			// throttle; flush rendering commands so the GPU readback sees the new frame.
			Viewport->Invalidate();
			Viewport->Draw(/*bShouldPresent*/ true);
			FlushRenderingCommands();

			TArray<FColor> Bitmap;
			if (!Viewport->ReadPixels(Bitmap) || Bitmap.Num() < Size.X * Size.Y)
			{
				return ErrorJson(TEXT("Failed to read viewport pixels."));
			}
			for (FColor& Pixel : Bitmap)
			{
				Pixel.A = 255;
			}

			// Optional integer downscale so inline payloads stay reasonable.
			double MaxWidthNumber = 1280.0;
			Args->TryGetNumberField(TEXT("maxWidth"), MaxWidthNumber);
			const int32 MaxWidth = FMath::Max(0, static_cast<int32>(MaxWidthNumber));

			int32 OutWidth = Size.X;
			int32 OutHeight = Size.Y;
			TArray<FColor> Scaled;
			const TArray<FColor>* Pixels = &Bitmap;
			if (MaxWidth > 0 && Size.X > MaxWidth)
			{
				const int32 Factor = FMath::DivideAndRoundUp(Size.X, MaxWidth);
				OutWidth = Size.X / Factor;
				OutHeight = Size.Y / Factor;
				Scaled.Reserve(OutWidth * OutHeight);
				for (int32 Y = 0; Y < OutHeight; ++Y)
				{
					for (int32 X = 0; X < OutWidth; ++X)
					{
						Scaled.Add(Bitmap[(Y * Factor) * Size.X + (X * Factor)]);
					}
				}
				Pixels = &Scaled;
			}

			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(OutWidth, OutHeight, *Pixels, Png);

			const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("screenshots"));
			IFileManager::Get().MakeDirectory(*Dir, true);
			const FString FileName = FString::Printf(TEXT("viewport_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
			FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, FileName));
			FPaths::MakePlatformFilename(FullPath);

			if (!FFileHelper::SaveArrayToFile(Png, *FullPath))
			{
				return ErrorJson(FString::Printf(TEXT("Failed to write screenshot: %s"), *FullPath));
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("path"), FullPath);
			Result->SetNumberField(TEXT("width"), OutWidth);
			Result->SetNumberField(TEXT("height"), OutHeight);
			Result->SetNumberField(TEXT("sourceWidth"), Size.X);
			Result->SetNumberField(TEXT("sourceHeight"), Size.Y);
			Result->SetNumberField(TEXT("bytes"), Png.Num());

			// Attach the PNG inline so vision-capable agents can see the viewport directly.
			// HandleToolsCall lifts "_imageContent" into a proper MCP image content item.
			bool bInline = true;
			Args->TryGetBoolField(TEXT("inline"), bInline);
			if (bInline)
			{
				TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
				Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
				Image->SetStringField(TEXT("data"), FBase64::Encode(Png.GetData(), static_cast<uint32>(Png.Num())));
				Result->SetObjectField(TEXT("_imageContent"), Image);
			}
			return SuccessJson(Result);
		}

		FString AddComponent(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("add_component is disabled while Play In Editor is active. Stop PIE first."));
			}

			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			AActor* Actor = FindActorByNameOrLabel(GetEditorWorld(), Name);
			if (!Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			FString ComponentClassText;
			if (!Args->TryGetStringField(TEXT("componentClass"), ComponentClassText) && !Args->TryGetStringField(TEXT("class"), ComponentClassText))
			{
				return ErrorJson(TEXT("Missing required field 'componentClass'."));
			}

			UClass* ComponentClass = ResolveClassByText(ComponentClassText);
			if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				return ErrorJson(FString::Printf(TEXT("Not a valid component class: %s"), *ComponentClassText));
			}
			if (ComponentClass->HasAnyClassFlags(CLASS_Abstract))
			{
				return ErrorJson(FString::Printf(TEXT("Component class is abstract: %s"), *ComponentClassText));
			}

			FString DesiredName;
			Args->TryGetStringField(TEXT("componentName"), DesiredName);
			const FName CompName = DesiredName.IsEmpty()
				? MakeUniqueObjectName(Actor, ComponentClass, ComponentClass->GetFName())
				: MakeUniqueObjectName(Actor, ComponentClass, FName(*DesiredName));

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "AddComponent", "MCP Add Component"));
			Actor->Modify();

			UActorComponent* NewComp = NewObject<UActorComponent>(Actor, ComponentClass, CompName, RF_Transactional);
			if (!NewComp)
			{
				Transaction.Cancel();
				return ErrorJson(TEXT("Failed to create component."));
			}
			NewComp->OnComponentCreated();

			if (USceneComponent* SceneComp = Cast<USceneComponent>(NewComp))
			{
				if (USceneComponent* Root = Actor->GetRootComponent())
				{
					SceneComp->SetupAttachment(Root);
				}
				else
				{
					Actor->SetRootComponent(SceneComp);
				}
			}

			Actor->AddInstanceComponent(NewComp);
			NewComp->RegisterComponent();
			Actor->MarkPackageDirty();

			TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
			Component->SetStringField(TEXT("name"), NewComp->GetName());
			Component->SetStringField(TEXT("class"), NewComp->GetClass()->GetName());

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("component"), Component);
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString AttachActor(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("attach_actor is disabled while Play In Editor is active. Stop PIE first."));
			}

			FString ChildName;
			if (!Args->TryGetStringField(TEXT("name"), ChildName)
				&& !Args->TryGetStringField(TEXT("child"), ChildName)
				&& !Args->TryGetStringField(TEXT("label"), ChildName))
			{
				return ErrorJson(TEXT("Missing required field 'name' (the actor to attach)."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Child = FindActorByNameOrLabel(World, ChildName);
			if (!Child)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *ChildName));
			}

			bool bDetach = false;
			Args->TryGetBoolField(TEXT("detach"), bDetach);
			if (bDetach)
			{
				FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "DetachActor", "MCP Detach Actor"));
				Child->Modify();
				Child->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
				Child->MarkPackageDirty();

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("detached"), true);
				Result->SetObjectField(TEXT("actor"), MakeActorObject(Child));
				return SuccessJson(Result);
			}

			FString ParentName;
			if (!Args->TryGetStringField(TEXT("parent"), ParentName))
			{
				return ErrorJson(TEXT("Missing required field 'parent'."));
			}

			AActor* Parent = FindActorByNameOrLabel(World, ParentName);
			if (!Parent)
			{
				return ErrorJson(FString::Printf(TEXT("Parent actor not found: %s"), *ParentName));
			}
			if (Parent == Child)
			{
				return ErrorJson(TEXT("Cannot attach an actor to itself."));
			}

			FString Socket;
			Args->TryGetStringField(TEXT("socket"), Socket);
			bool bKeepWorld = true;
			Args->TryGetBoolField(TEXT("keepWorldTransform"), bKeepWorld);
			const FAttachmentTransformRules Rules = bKeepWorld
				? FAttachmentTransformRules::KeepWorldTransform
				: FAttachmentTransformRules::KeepRelativeTransform;

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "AttachActor", "MCP Attach Actor"));
			Child->Modify();
			Parent->Modify();
			const bool bAttached = Child->AttachToActor(Parent, Rules, Socket.IsEmpty() ? NAME_None : FName(*Socket));
			if (!bAttached)
			{
				Transaction.Cancel();
				return ErrorJson(TEXT("AttachToActor failed (the actor may lack a root scene component)."));
			}
			Child->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("child"), Child->GetName());
			Result->SetStringField(TEXT("parent"), Parent->GetName());
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Child));
			return SuccessJson(Result);
		}

		FString DuplicateActor(const TSharedPtr<FJsonObject>& Args)
		{
			if (IsPlayInEditorActive())
			{
				return ErrorJson(TEXT("duplicate_actor is disabled while Play In Editor is active. Stop PIE first."));
			}

			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Source = FindActorByNameOrLabel(World, Name);
			if (!Source)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			// Copy the source transform by default; allow an explicit override/offset.
			FVector Location = Source->GetActorLocation();
			FRotator Rotation = Source->GetActorRotation();
			TryGetVectorField(Args, TEXT("location"), Location);
			TryGetRotatorField(Args, TEXT("rotation"), Rotation);

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "DuplicateActor", "MCP Duplicate Actor"));
			World->Modify();

			// Template spawn copies the source actor's properties and components.
			const FTransform SpawnTransform(Rotation, Location, Source->GetActorScale3D());
			FActorSpawnParameters SpawnParams;
			SpawnParams.Template = Source;
			AActor* NewActor = World->SpawnActor(Source->GetClass(), &SpawnTransform, SpawnParams);
			if (!NewActor)
			{
				Transaction.Cancel();
				return ErrorJson(FString::Printf(TEXT("Failed to duplicate actor: %s"), *Name));
			}

			FString NewLabel;
			if (Args->TryGetStringField(TEXT("newLabel"), NewLabel) && !NewLabel.IsEmpty())
			{
				NewActor->SetActorLabel(NewLabel);
			}
			NewActor->MarkPackageDirty();
			if (GEditor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(NewActor, true, true);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("sourceName"), Source->GetName());
			Result->SetObjectField(TEXT("actor"), MakeActorObject(NewActor));
			return SuccessJson(Result);
		}
	}
}
