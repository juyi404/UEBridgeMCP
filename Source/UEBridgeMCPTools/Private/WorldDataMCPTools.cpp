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
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

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

	bool TryValueToNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutNumber = Value->AsNumber();
			return true;
		}
		if (Value->Type == EJson::String)
		{
			OutNumber = FCString::Atod(*Value->AsString());
			return true;
		}
		return false;
	}

	bool TryValueToBool(const TSharedPtr<FJsonValue>& Value, bool& bOutValue)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Boolean)
		{
			bOutValue = Value->AsBool();
			return true;
		}
		if (Value->Type == EJson::String)
		{
			const FString Text = Value->AsString().ToLower();
			if (Text == TEXT("true") || Text == TEXT("1") || Text == TEXT("yes"))
			{
				bOutValue = true;
				return true;
			}
			if (Text == TEXT("false") || Text == TEXT("0") || Text == TEXT("no"))
			{
				bOutValue = false;
				return true;
			}
		}
		return false;
	}

	bool TryValueToVector(const TSharedPtr<FJsonValue>& Value, FVector& OutVector)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
		{
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (TryValueToNumber((*Array)[0], X) && TryValueToNumber((*Array)[1], Y) && TryValueToNumber((*Array)[2], Z))
			{
				OutVector = FVector(X, Y, Z);
				return true;
			}
		}

		TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid())
		{
			double X = OutVector.X;
			double Y = OutVector.Y;
			double Z = OutVector.Z;
			TryGetNumberFieldCaseInsensitive(Object, TEXT("x"), TEXT("X"), X);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("y"), TEXT("Y"), Y);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("z"), TEXT("Z"), Z);
			OutVector = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	bool TryValueToRotator(const TSharedPtr<FJsonValue>& Value, FRotator& OutRotator)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			if (TryValueToNumber((*Array)[0], Pitch) && TryValueToNumber((*Array)[1], Yaw) && TryValueToNumber((*Array)[2], Roll))
			{
				OutRotator = FRotator(Pitch, Yaw, Roll);
				return true;
			}
		}

		TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid())
		{
			double Pitch = OutRotator.Pitch;
			double Yaw = OutRotator.Yaw;
			double Roll = OutRotator.Roll;
			TryGetNumberFieldCaseInsensitive(Object, TEXT("pitch"), TEXT("Pitch"), Pitch);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("yaw"), TEXT("Yaw"), Yaw);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("roll"), TEXT("Roll"), Roll);
			OutRotator = FRotator(Pitch, Yaw, Roll);
			return true;
		}
		return false;
	}

	bool TryValueToLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array) && Array && Array->Num() >= 3)
		{
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 1.0;
			if (TryValueToNumber((*Array)[0], R) && TryValueToNumber((*Array)[1], G) && TryValueToNumber((*Array)[2], B))
			{
				if (Array->Num() >= 4)
				{
					TryValueToNumber((*Array)[3], A);
				}
				OutColor = FLinearColor(R, G, B, A);
				return true;
			}
		}

		TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid())
		{
			double R = OutColor.R;
			double G = OutColor.G;
			double B = OutColor.B;
			double A = OutColor.A;
			TryGetNumberFieldCaseInsensitive(Object, TEXT("r"), TEXT("R"), R);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("g"), TEXT("G"), G);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("b"), TEXT("B"), B);
			TryGetNumberFieldCaseInsensitive(Object, TEXT("a"), TEXT("A"), A);
			OutColor = FLinearColor(R, G, B, A);
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

		// Object paths are the preferred mutation target. They are unique within
		// the loaded world, unlike editor labels, and are returned as actorPath by
		// every actor-reading tool.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (IsValid(Actor) && Actor->GetPathName().Equals(NameOrLabel, ESearchCase::CaseSensitive))
			{
				return Actor;
			}
		}

		AActor* Candidate = nullptr;
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
				// Never let a mutating request silently pick the first duplicate label.
				// Callers can retry with the actorPath emitted by list/details tools.
				if (Candidate && Candidate != Actor)
				{
					return nullptr;
				}
				Candidate = Actor;
			}
		}

		return Candidate;
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

	UClass* ResolveObjectClass(const FString& ClassText)
	{
		if (ClassText.IsEmpty())
		{
			return nullptr;
		}

		UClass* ObjectClass = FindObject<UClass>(nullptr, *ClassText);
		if (!ObjectClass)
		{
			ObjectClass = LoadObject<UClass>(nullptr, *ClassText);
		}

		if (!ObjectClass && !ClassText.StartsWith(TEXT("/")))
		{
			for (const FString& ModulePath : { TEXT("/Script/Engine."), TEXT("/Script/CoreUObject."), TEXT("/Script/PCG.") })
			{
				ObjectClass = FindObject<UClass>(nullptr, *(ModulePath + ClassText));
				if (!ObjectClass)
				{
					ObjectClass = LoadObject<UClass>(nullptr, *(ModulePath + ClassText));
				}
				if (ObjectClass)
				{
					break;
				}
			}
		}

		return ObjectClass && ObjectClass->IsChildOf(UObject::StaticClass()) ? ObjectClass : nullptr;
	}

	bool SplitAssetPath(FString InPath, FString& OutPackageName, FString& OutAssetName, FString& OutError)
	{
		InPath.TrimStartAndEndInline();
		if (InPath.IsEmpty())
		{
			OutError = TEXT("assetPath is required.");
			return false;
		}
		if (!InPath.StartsWith(TEXT("/Game/")))
		{
			OutError = TEXT("assetPath must be under /Game.");
			return false;
		}

		if (InPath.Contains(TEXT(".")))
		{
			InPath.Split(TEXT("."), &InPath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart);
		}

		OutPackageName = InPath;
		OutAssetName = FPaths::GetBaseFilename(InPath);
		FText PackageNameError;
		if (OutAssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(OutPackageName, false, &PackageNameError))
		{
			if (OutError.IsEmpty())
			{
				OutError = PackageNameError.IsEmpty()
					? TEXT("assetPath is not a valid long package name.")
					: PackageNameError.ToString();
			}
			return false;
		}
		return true;
	}

	bool SavePackages(const TArray<UPackage*>& Packages)
	{
		if (Packages.Num() == 0)
		{
			return true;
		}

		TArray<UPackage*> SaveList = Packages;
		return FEditorFileUtils::PromptForCheckoutAndSave(SaveList, false, false) == FEditorFileUtils::PR_Success;
	}

	bool SetPropertyFromJson(UObject* Target, FProperty* Property, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Target || !Property)
		{
			OutError = TEXT("Target object or property is invalid.");
			return false;
		}

		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient))
		{
			OutError = FString::Printf(TEXT("Property '%s' is not editable through MCP."), *Property->GetName());
			return false;
		}

		void* Address = Property->ContainerPtrToValuePtr<void>(Target);
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (!TryValueToBool(Value, bValue))
			{
				OutError = TEXT("Expected a boolean value.");
				return false;
			}
			BoolProperty->SetPropertyValue(Address, bValue);
			return true;
		}
		if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			double Number = 0.0;
			if (!TryValueToNumber(Value, Number))
			{
				OutError = TEXT("Expected a number value.");
				return false;
			}
			IntProperty->SetPropertyValue(Address, static_cast<int32>(Number));
			return true;
		}
		if (FInt64Property* Int64Property = CastField<FInt64Property>(Property))
		{
			double Number = 0.0;
			if (!TryValueToNumber(Value, Number))
			{
				OutError = TEXT("Expected a number value.");
				return false;
			}
			Int64Property->SetPropertyValue(Address, static_cast<int64>(Number));
			return true;
		}
		if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			double Number = 0.0;
			if (!TryValueToNumber(Value, Number))
			{
				OutError = TEXT("Expected a number value.");
				return false;
			}
			FloatProperty->SetPropertyValue(Address, static_cast<float>(Number));
			return true;
		}
		if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			double Number = 0.0;
			if (!TryValueToNumber(Value, Number))
			{
				OutError = TEXT("Expected a number value.");
				return false;
			}
			DoubleProperty->SetPropertyValue(Address, Number);
			return true;
		}
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const FString Text = Value.IsValid() ? Value->AsString() : FString();
			int64 EnumValue = EnumProperty->GetEnum()->GetValueByNameString(Text);
			if (EnumValue == INDEX_NONE)
			{
				double Number = 0.0;
				if (!TryValueToNumber(Value, Number))
				{
					OutError = FString::Printf(TEXT("Unknown enum value '%s'."), *Text);
					return false;
				}
				EnumValue = static_cast<int64>(Number);
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Address, EnumValue);
			return true;
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			double Number = 0.0;
			if (!TryValueToNumber(Value, Number))
			{
				const FString Text = Value.IsValid() ? Value->AsString() : FString();
				if (ByteProperty->Enum)
				{
					const int64 EnumValue = ByteProperty->Enum->GetValueByNameString(Text);
					if (EnumValue != INDEX_NONE)
					{
						ByteProperty->SetPropertyValue(Address, static_cast<uint8>(EnumValue));
						return true;
					}
				}
				OutError = TEXT("Expected a byte number or enum name.");
				return false;
			}
			ByteProperty->SetPropertyValue(Address, static_cast<uint8>(Number));
			return true;
		}
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(Address, Value.IsValid() ? Value->AsString() : FString());
			return true;
		}
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(Address, FName(*(Value.IsValid() ? Value->AsString() : FString())));
			return true;
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			TextProperty->SetPropertyValue(Address, FText::FromString(Value.IsValid() ? Value->AsString() : FString()));
			return true;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FVector>::Get())
			{
				FVector Vector = *static_cast<FVector*>(Address);
				if (!TryValueToVector(Value, Vector))
				{
					OutError = TEXT("Expected vector object {x,y,z} or array [x,y,z].");
					return false;
				}
				*static_cast<FVector*>(Address) = Vector;
				return true;
			}
			if (StructProperty->Struct == TBaseStructure<FRotator>::Get())
			{
				FRotator Rotator = *static_cast<FRotator*>(Address);
				if (!TryValueToRotator(Value, Rotator))
				{
					OutError = TEXT("Expected rotator object {pitch,yaw,roll} or array [pitch,yaw,roll].");
					return false;
				}
				*static_cast<FRotator*>(Address) = Rotator;
				return true;
			}
			if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
			{
				FLinearColor Color = *static_cast<FLinearColor*>(Address);
				if (!TryValueToLinearColor(Value, Color))
				{
					OutError = TEXT("Expected color object {r,g,b,a} or array [r,g,b,a].");
					return false;
				}
				*static_cast<FLinearColor*>(Address) = Color;
				return true;
			}
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const FString ObjectPath = Value.IsValid() ? Value->AsString() : FString();
			UObject* ObjectValue = ObjectPath.IsEmpty() ? nullptr : StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *NormalizeAssetObjectPath(ObjectPath));
			if (!ObjectPath.IsEmpty() && !ObjectValue)
			{
				OutError = FString::Printf(TEXT("Could not load object for property '%s': %s"), *Property->GetName(), *ObjectPath);
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(Address, ObjectValue);
			return true;
		}

		OutError = FString::Printf(TEXT("Property type for '%s' is not supported yet."), *Property->GetName());
		return false;
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
		ActorJson->SetStringField(TEXT("actorPath"), Actor->GetPathName());

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
		FString GetToolDefinitionsJson()
		{
			static const FString LocalToolsJson = FString(TEXT(R"JSON([
{"name":"get_current_project_info","description":"Return the UE project identity and MCP endpoint for this editor session.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Project Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"list_level_actors","description":"List actors in the currently loaded editor world with transforms, folder, mobility, and selection state.","inputSchema":{"type":"object","properties":{"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"nameContains":{"type":"string","description":"Optional case-insensitive actor name or label substring."},"selectedOnly":{"type":"boolean","description":"When true, only return currently selected actors."},"maxResults":{"type":"number","description":"Maximum returned actors. Default 200, capped at 1000."}}},"annotations":{"title":"List Level Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_selected_actors","description":"Return the actors currently selected in the editor viewport/outliner.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Selected Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_actor_details","description":"Read one actor in depth: transform, tags, and its components with classes and relative transforms.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}},"required":["name"]},"annotations":{"title":"Get Actor Details","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_assets","description":"Search assets under a content path without loading them.","inputSchema":{"type":"object","properties":{"searchTerm":{"type":"string","description":"Optional case-insensitive asset name or path substring."},"classFilter":{"type":"string","description":"Optional class-name substring such as StaticMesh, Blueprint, World, Material."},"path":{"type":"string","description":"Content root to search. Default /Game."},"maxResults":{"type":"number","description":"Maximum returned assets. Default 50, capped at 500."}}},"annotations":{"title":"Find Assets","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_asset","description":"Read basic Asset Registry metadata for one asset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Asset object path or package path, for example /Game/Foo/Bar.Bar or /Game/Foo/Bar."}},"required":["assetPath"]},"annotations":{"title":"Read Asset","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_content_summary","description":"Summarize the Asset Registry under a content path: total asset count and a histogram of asset counts by class.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Content root to summarize. Default /Game."},"maxClasses":{"type":"number","description":"Maximum class buckets returned. Default 30, capped at 200."}}},"annotations":{"title":"Get Content Summary","readOnlyHint":true,"openWorldHint":false}},
{"name":"select_actor","description":"Select an actor in the editor by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}}},"annotations":{"title":"Select Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"spawn_actor","description":"Spawn an actor into the current editor world. Use staticMeshPath to create a StaticMeshActor.","inputSchema":{"type":"object","properties":{"class":{"type":"string","description":"Actor class name/path. Default Actor. Blueprint asset paths are supported."},"staticMeshPath":{"type":"string","description":"Optional StaticMesh asset path. When supplied, spawns a StaticMeshActor."},"label":{"type":"string","description":"Optional editor label."},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}}},"annotations":{"title":"Spawn Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
)JSON")) + TEXT(R"JSON(
{"name":"transform_actor","description":"Set an actor world transform by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}},"required":["name"]},"annotations":{"title":"Transform Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"delete_actor","description":"Delete an actor from the current editor world by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}},"required":["name"]},"annotations":{"title":"Delete Actor","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"attach_actor","description":"Attach one actor to another in the editor world.","inputSchema":{"type":"object","properties":{"child":{"type":"string","description":"Child actor name or label."},"childName":{"type":"string"},"parent":{"type":"string","description":"Parent actor name or label."},"parentName":{"type":"string"},"socket":{"type":"string"},"keepWorldTransform":{"type":"boolean","description":"Default true."}},"required":["child","parent"]},"annotations":{"title":"Attach Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_actor_property","description":"Set an editable actor or component property using reflection. Supports primitive values, enum names, FVector, FRotator, FLinearColor, and UObject asset references.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."},"component":{"type":"string","description":"Optional component name or class name."},"property":{"type":"string","description":"Property name."},"value":{"description":"JSON value compatible with the property type."}},"required":["name","property","value"]},"annotations":{"title":"Set Actor Property","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"save_current_level","description":"Save the current level package to disk.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Save Current Level","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_asset","description":"Create a UObject asset under /Game with the supplied class. Useful for simple data assets and reflected asset classes such as PCGGraph.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Long package path under /Game, for example /Game/MCP/NewAsset."},"path":{"type":"string","description":"Alias for assetPath."},"class":{"type":"string","description":"UObject class name or path. Default DataAsset."},"save":{"type":"boolean","description":"When true, save the created asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create Asset","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_blueprint_asset","description":"Create a Blueprint asset under /Game using a reflected parent class. Graph node construction is not exposed yet.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Long package path under /Game."},"path":{"type":"string","description":"Alias for assetPath."},"parentClass":{"type":"string","description":"Parent class name/path. Default Actor."},"save":{"type":"boolean","description":"When true, save the created asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create Blueprint Asset","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"modify_material_instance","description":"Modify scalar and vector parameters on a MaterialInstanceConstant asset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"MaterialInstanceConstant object path or package path."},"path":{"type":"string","description":"Alias for assetPath."},"scalarParameters":{"type":"object","additionalProperties":{"type":"number"}},"vectorParameters":{"type":"object","description":"Map of parameter name to {r,g,b,a} object or [r,g,b,a] array."},"save":{"type":"boolean","description":"When true, save the modified asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Modify Material Instance","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_pcg_graph_from_recipe","description":"Create a PCGGraph asset under /Game from recipe metadata. If sourceGraph/source_graph is supplied, duplicates that existing PCGGraph including nodes and edges; otherwise creates an empty PCGGraph container and reports recipeApplied=false.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"PCGGraph package path under /Game."},"path":{"type":"string","description":"Alias for assetPath."},"recipe_id":{"type":"string"},"id":{"type":"string"},"sourceGraph":{"type":"string","description":"Optional source PCGGraph asset path to duplicate."},"source_graph":{"type":"string","description":"Alias for sourceGraph."},"recipe":{"type":"object","description":"Optional recipe object containing recipe_id/id and source_graph."},"save":{"type":"boolean","description":"When true, save the created graph asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create PCG Graph From Recipe","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"get_mcp_governance","description":"Return the enforced UEBridgeMCP tool risk matrix, interactive-approval policy, and redacted audit-log location.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"MCP Governance","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_mcp_job_status","description":"Read the status and final structured result of an asynchronous MCP tool job. Mutating tools return a jobId and approvalId immediately, then remain awaiting_approval until the Unreal Editor user decides. Read-only long-running tools may use arguments.worlddataAsync=true.","inputSchema":{"type":"object","properties":{"jobId":{"type":"string","description":"Job identifier returned by an asynchronous or approval-gated tool call."}},"required":["jobId"]},"annotations":{"title":"Get MCP Job Status","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_codex_policy_snapshot","description":"Read a redacted snapshot of explicit local Codex policy: approval_policy, sandbox_mode, active profile, model, and non-secret MCP enablement/type metadata. Command lines, arguments, paths, URLs, environment values, hidden prompts, and runtime secrets are omitted.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Codex Policy Snapshot","readOnlyHint":true,"openWorldHint":false}}
])JSON");
			return LocalToolsJson;
		}

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

		FString TransformActor(const TSharedPtr<FJsonObject>& Args)
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

			FVector Location = Actor->GetActorLocation();
			FRotator Rotation = Actor->GetActorRotation();
			FVector Scale = Actor->GetActorScale3D();
			const bool bHasLocation = TryGetVectorField(Args, TEXT("location"), Location);
			const bool bHasRotation = TryGetRotatorField(Args, TEXT("rotation"), Rotation);
			const bool bHasScale = TryGetVectorField(Args, TEXT("scale"), Scale);
			if (!bHasLocation && !bHasRotation && !bHasScale)
			{
				return ErrorJson(TEXT("Provide at least one of location, rotation, or scale."));
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "TransformActor", "MCP Transform Actor"));
			Actor->Modify();
			if (bHasLocation || bHasRotation)
			{
				Actor->SetActorLocationAndRotation(Location, Rotation);
			}
			if (bHasScale)
			{
				Actor->SetActorScale3D(Scale);
			}
			Actor->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString DeleteActor(const TSharedPtr<FJsonObject>& Args)
		{
			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Actor = FindActorByNameOrLabel(World, Name);
			if (!World || !Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			TSharedPtr<FJsonObject> ActorBeforeDelete = MakeActorObject(Actor);
			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "DeleteActor", "MCP Delete Actor"));
			World->Modify();
			Actor->Modify();
			if (GEditor)
			{
				GEditor->SelectActor(Actor, false, false);
			}
			const bool bDestroyed = World->EditorDestroyActor(Actor, true);
			World->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("deleted"), bDestroyed);
			Result->SetObjectField(TEXT("actor"), ActorBeforeDelete);
			return bDestroyed ? SuccessJson(Result) : ErrorJson(TEXT("EditorDestroyActor failed."));
		}

		FString AttachActor(const TSharedPtr<FJsonObject>& Args)
		{
			FString ChildName;
			if (!Args->TryGetStringField(TEXT("child"), ChildName)
				&& !Args->TryGetStringField(TEXT("childName"), ChildName)
				&& !Args->TryGetStringField(TEXT("name"), ChildName))
			{
				return ErrorJson(TEXT("Missing required field 'child'."));
			}

			FString ParentName;
			if (!Args->TryGetStringField(TEXT("parent"), ParentName)
				&& !Args->TryGetStringField(TEXT("parentName"), ParentName))
			{
				return ErrorJson(TEXT("Missing required field 'parent'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Child = FindActorByNameOrLabel(World, ChildName);
			AActor* Parent = FindActorByNameOrLabel(World, ParentName);
			if (!Child)
			{
				return ErrorJson(FString::Printf(TEXT("Child actor not found: %s"), *ChildName));
			}
			if (!Parent)
			{
				return ErrorJson(FString::Printf(TEXT("Parent actor not found: %s"), *ParentName));
			}
			if (Child == Parent)
			{
				return ErrorJson(TEXT("An actor cannot be attached to itself."));
			}

			bool bKeepWorldTransform = true;
			Args->TryGetBoolField(TEXT("keepWorldTransform"), bKeepWorldTransform);

			FString Socket;
			Args->TryGetStringField(TEXT("socket"), Socket);

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "AttachActor", "MCP Attach Actor"));
			Child->Modify();
			Parent->Modify();
			const FAttachmentTransformRules Rules = bKeepWorldTransform
				? FAttachmentTransformRules::KeepWorldTransform
				: FAttachmentTransformRules::KeepRelativeTransform;
			const bool bAttached = Child->AttachToActor(Parent, Rules, Socket.IsEmpty() ? NAME_None : FName(*Socket));
			Child->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("attached"), bAttached);
			Result->SetObjectField(TEXT("child"), MakeActorObject(Child));
			Result->SetObjectField(TEXT("parent"), MakeActorObject(Parent));
			return bAttached ? SuccessJson(Result) : ErrorJson(TEXT("AttachToActor failed."));
		}

		FString SetActorProperty(const TSharedPtr<FJsonObject>& Args)
		{
			FString Name;
			if (!Args->TryGetStringField(TEXT("name"), Name) && !Args->TryGetStringField(TEXT("label"), Name))
			{
				return ErrorJson(TEXT("Missing required field 'name'."));
			}

			FString PropertyName;
			if (!Args->TryGetStringField(TEXT("property"), PropertyName))
			{
				return ErrorJson(TEXT("Missing required field 'property'."));
			}

			const TSharedPtr<FJsonValue> Value = Args->TryGetField(TEXT("value"));
			if (!Value.IsValid())
			{
				return ErrorJson(TEXT("Missing required field 'value'."));
			}

			UWorld* World = GetEditorWorld();
			AActor* Actor = FindActorByNameOrLabel(World, Name);
			if (!Actor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *Name));
			}

			UObject* Target = Actor;
			FString ComponentName;
			if (Args->TryGetStringField(TEXT("component"), ComponentName) && !ComponentName.IsEmpty())
			{
				Target = nullptr;
				TInlineComponentArray<UActorComponent*> Components(Actor);
				for (UActorComponent* Component : Components)
				{
					if (IsValid(Component)
						&& (Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
							|| Component->GetClass()->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)))
					{
						Target = Component;
						break;
					}
				}
				if (!Target)
				{
					return ErrorJson(FString::Printf(TEXT("Component not found on actor '%s': %s"), *Name, *ComponentName));
				}
			}

			FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*PropertyName));
			if (!Property)
			{
				return ErrorJson(FString::Printf(TEXT("Property '%s' was not found on %s."), *PropertyName, *Target->GetName()));
			}

			FString Error;
			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "SetActorProperty", "MCP Set Actor Property"));
			Target->Modify();
			Target->PreEditChange(Property);
			if (!SetPropertyFromJson(Target, Property, Value, Error))
			{
				return ErrorJson(Error);
			}
			FPropertyChangedEvent ChangeEvent(Property);
			Target->PostEditChangeProperty(ChangeEvent);
			Target->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Target->GetPathName());
			Result->SetStringField(TEXT("property"), PropertyName);
			Result->SetObjectField(TEXT("actor"), MakeActorObject(Actor));
			return SuccessJson(Result);
		}

		FString SaveCurrentLevel(const TSharedPtr<FJsonObject>& Args)
		{
			UWorld* World = GetEditorWorld();
			if (!World)
			{
				return ErrorJson(TEXT("Editor world is not available."));
			}

			UPackage* Package = World->GetOutermost();
			TArray<UPackage*> PackagesToSave;
			PackagesToSave.Add(Package);
			const bool bSaved = SavePackages(PackagesToSave);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetStringField(TEXT("levelPackage"), Package ? Package->GetName() : FString());
			return bSaved ? SuccessJson(Result) : ErrorJson(TEXT("Failed to save current level package."));
		}

		FString CreateAsset(const TSharedPtr<FJsonObject>& Args)
		{
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("assetPath"), AssetPath) && !Args->TryGetStringField(TEXT("path"), AssetPath))
			{
				return ErrorJson(TEXT("Missing required field 'assetPath'."));
			}

			FString ClassText = TEXT("DataAsset");
			Args->TryGetStringField(TEXT("class"), ClassText);
			UClass* AssetClass = ResolveObjectClass(ClassText);
			if (!AssetClass || AssetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				return ErrorJson(FString::Printf(TEXT("Asset class not found or not instantiable: %s"), *ClassText));
			}

			FString PackageName;
			FString AssetName;
			FString Error;
			if (!SplitAssetPath(AssetPath, PackageName, AssetName, Error))
			{
				return ErrorJson(Error);
			}

			if (StaticFindObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(PackageName)))
			{
				return ErrorJson(FString::Printf(TEXT("Asset already exists: %s"), *PackageName));
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "CreateAsset", "MCP Create Asset"));
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return ErrorJson(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
			}
			Package->FullyLoad();

			UObject* Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!Asset)
			{
				return ErrorJson(FString::Printf(TEXT("Failed to create asset object: %s"), *AssetName));
			}

			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();

			bool bSave = false;
			Args->TryGetBoolField(TEXT("save"), bSave);
			bool bSaved = false;
			if (bSave)
			{
				TArray<UPackage*> PackagesToSave;
				PackagesToSave.Add(Package);
				bSaved = SavePackages(PackagesToSave);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
			Result->SetStringField(TEXT("packageName"), PackageName);
			Result->SetStringField(TEXT("class"), AssetClass->GetName());
			Result->SetBoolField(TEXT("saved"), bSaved);
			return SuccessJson(Result);
		}

		FString CreateBlueprintAsset(const TSharedPtr<FJsonObject>& Args)
		{
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("assetPath"), AssetPath) && !Args->TryGetStringField(TEXT("path"), AssetPath))
			{
				return ErrorJson(TEXT("Missing required field 'assetPath'."));
			}

			FString ParentClassText = TEXT("Actor");
			Args->TryGetStringField(TEXT("parentClass"), ParentClassText);
			UClass* ParentClass = ResolveObjectClass(ParentClassText);
			if (!ParentClass)
			{
				return ErrorJson(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassText));
			}

			FString PackageName;
			FString AssetName;
			FString Error;
			if (!SplitAssetPath(AssetPath, PackageName, AssetName, Error))
			{
				return ErrorJson(Error);
			}
			if (StaticFindObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(PackageName)))
			{
				return ErrorJson(FString::Printf(TEXT("Asset already exists: %s"), *PackageName));
			}

			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "CreateBlueprintAsset", "MCP Create Blueprint Asset"));
			UPackage* Package = CreatePackage(*PackageName);
			if (!Package)
			{
				return ErrorJson(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
			}

			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*AssetName),
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
			if (!Blueprint)
			{
				return ErrorJson(TEXT("Failed to create Blueprint asset."));
			}

			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();

			bool bSave = false;
			Args->TryGetBoolField(TEXT("save"), bSave);
			bool bSaved = false;
			if (bSave)
			{
				TArray<UPackage*> PackagesToSave;
				PackagesToSave.Add(Package);
				bSaved = SavePackages(PackagesToSave);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("packageName"), PackageName);
			Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetStringField(TEXT("note"), TEXT("Blueprint asset creation is supported; graph node construction is not exposed yet."));
			return SuccessJson(Result);
		}

		FString ModifyMaterialInstance(const TSharedPtr<FJsonObject>& Args)
		{
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("assetPath"), AssetPath) && !Args->TryGetStringField(TEXT("path"), AssetPath))
			{
				return ErrorJson(TEXT("Missing required field 'assetPath'."));
			}

			UMaterialInstanceConstant* MaterialInstance = LoadObject<UMaterialInstanceConstant>(nullptr, *NormalizeAssetObjectPath(AssetPath));
			if (!MaterialInstance)
			{
				return ErrorJson(FString::Printf(TEXT("MaterialInstanceConstant not found: %s"), *AssetPath));
			}

			int32 ChangedCount = 0;
			FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "ModifyMaterialInstance", "MCP Modify Material Instance"));
			MaterialInstance->Modify();

			const TSharedPtr<FJsonObject>* ScalarParams = nullptr;
			if (Args->TryGetObjectField(TEXT("scalarParameters"), ScalarParams) && ScalarParams && ScalarParams->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ScalarParams)->Values)
				{
					double Number = 0.0;
					if (!TryValueToNumber(Pair.Value, Number))
					{
						return ErrorJson(FString::Printf(TEXT("Scalar parameter '%s' must be numeric."), *Pair.Key));
					}
					MaterialInstance->SetScalarParameterValueEditorOnly(FName(*Pair.Key), static_cast<float>(Number));
					++ChangedCount;
				}
			}

			const TSharedPtr<FJsonObject>* VectorParams = nullptr;
			if (Args->TryGetObjectField(TEXT("vectorParameters"), VectorParams) && VectorParams && VectorParams->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*VectorParams)->Values)
				{
					FLinearColor Color = FLinearColor::White;
					if (!TryValueToLinearColor(Pair.Value, Color))
					{
						return ErrorJson(FString::Printf(TEXT("Vector parameter '%s' must be {r,g,b,a} or [r,g,b,a]."), *Pair.Key));
					}
					MaterialInstance->SetVectorParameterValueEditorOnly(FName(*Pair.Key), Color);
					++ChangedCount;
				}
			}

			MaterialInstance->PostEditChange();
			MaterialInstance->MarkPackageDirty();

			bool bSave = false;
			Args->TryGetBoolField(TEXT("save"), bSave);
			bool bSaved = false;
			if (bSave)
			{
				TArray<UPackage*> PackagesToSave;
				PackagesToSave.Add(MaterialInstance->GetOutermost());
				bSaved = SavePackages(PackagesToSave);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("assetPath"), MaterialInstance->GetPathName());
			Result->SetNumberField(TEXT("changedCount"), ChangedCount);
			Result->SetBoolField(TEXT("saved"), bSaved);
			return SuccessJson(Result);
		}

		FString CreatePcgGraphFromRecipe(const TSharedPtr<FJsonObject>& Args)
		{
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("assetPath"), AssetPath) && !Args->TryGetStringField(TEXT("path"), AssetPath))
			{
				return ErrorJson(TEXT("Missing required field 'assetPath'."));
			}

			bool bSave = false;
			Args->TryGetBoolField(TEXT("save"), bSave);

			FString RecipeId;
			Args->TryGetStringField(TEXT("recipe_id"), RecipeId);
			if (RecipeId.IsEmpty())
			{
				Args->TryGetStringField(TEXT("id"), RecipeId);
			}

			FString SourceGraph;
			if (!Args->TryGetStringField(TEXT("sourceGraph"), SourceGraph))
			{
				Args->TryGetStringField(TEXT("source_graph"), SourceGraph);
			}
			const TSharedPtr<FJsonObject>* RecipeObject = nullptr;
			if (SourceGraph.IsEmpty() && Args->TryGetObjectField(TEXT("recipe"), RecipeObject) && RecipeObject && RecipeObject->IsValid())
			{
				if (!(*RecipeObject)->TryGetStringField(TEXT("source_graph"), SourceGraph))
				{
					(*RecipeObject)->TryGetStringField(TEXT("sourceGraph"), SourceGraph);
				}
				if (RecipeId.IsEmpty())
				{
					if (!(*RecipeObject)->TryGetStringField(TEXT("recipe_id"), RecipeId))
					{
						(*RecipeObject)->TryGetStringField(TEXT("id"), RecipeId);
					}
				}
			}

			if (!SourceGraph.IsEmpty())
			{
				UObject* SourceObject = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(SourceGraph));
				if (!SourceObject)
				{
					return ErrorJson(FString::Printf(TEXT("sourceGraph could not be loaded: %s"), *SourceGraph));
				}
				if (!SourceObject->GetClass()->GetName().Contains(TEXT("PCGGraph"), ESearchCase::IgnoreCase))
				{
					return ErrorJson(FString::Printf(TEXT("sourceGraph is not a PCGGraph asset: %s"), *SourceObject->GetPathName()));
				}

				FString PackageName;
				FString AssetName;
				FString Error;
				if (!SplitAssetPath(AssetPath, PackageName, AssetName, Error))
				{
					return ErrorJson(Error);
				}
				if (StaticFindObject(UObject::StaticClass(), nullptr, *NormalizeAssetObjectPath(PackageName)))
				{
					return ErrorJson(FString::Printf(TEXT("Asset already exists: %s"), *PackageName));
				}

				FScopedTransaction Transaction(NSLOCTEXT("UEBridgeMCP", "CreatePcgGraphFromRecipe", "MCP Create PCG Graph From Recipe"));
				UPackage* Package = CreatePackage(*PackageName);
				if (!Package)
				{
					return ErrorJson(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
				}
				Package->FullyLoad();

				UObject* DuplicatedGraph = StaticDuplicateObject(SourceObject, Package, FName(*AssetName));
				if (!DuplicatedGraph)
				{
					return ErrorJson(TEXT("Failed to duplicate source PCG graph."));
				}
				DuplicatedGraph->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
				DuplicatedGraph->ClearFlags(RF_Transient);
				FAssetRegistryModule::AssetCreated(DuplicatedGraph);
				Package->MarkPackageDirty();

				bool bSaved = false;
				if (bSave)
				{
					TArray<UPackage*> PackagesToSave;
					PackagesToSave.Add(Package);
					bSaved = SavePackages(PackagesToSave);
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("success"), true);
				Result->SetStringField(TEXT("assetPath"), DuplicatedGraph->GetPathName());
				Result->SetStringField(TEXT("packageName"), PackageName);
				Result->SetStringField(TEXT("sourceGraph"), SourceObject->GetPathName());
				Result->SetStringField(TEXT("recipeId"), RecipeId);
				Result->SetBoolField(TEXT("recipeApplied"), true);
				Result->SetStringField(TEXT("buildMethod"), TEXT("duplicated_source_graph"));
				Result->SetBoolField(TEXT("sceneBindingApplied"), false);
				Result->SetBoolField(TEXT("saved"), bSaved);
				Result->SetStringField(TEXT("note"), TEXT("Duplicated the recipe source_graph PCGGraph. Scene input binding and generated node synthesis are still separate steps."));
				return JsonObjectToString(Result);
			}

			TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
			CreateArgs->SetStringField(TEXT("assetPath"), AssetPath);
			CreateArgs->SetStringField(TEXT("class"), TEXT("PCGGraph"));
			CreateArgs->SetBoolField(TEXT("save"), bSave);

			FString Created = CreateAsset(CreateArgs);
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Created);
			TSharedPtr<FJsonObject> Result;
			if (!FJsonSerializer::Deserialize(Reader, Result) || !Result.IsValid())
			{
				return Created;
			}

			bool bSuccess = true;
			if (Result->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
			{
				return Created;
			}

			Result->SetStringField(TEXT("recipeId"), RecipeId);
			Result->SetBoolField(TEXT("recipeApplied"), false);
			Result->SetStringField(TEXT("buildMethod"), TEXT("empty_pcg_graph_asset"));
			Result->SetBoolField(TEXT("sceneBindingApplied"), false);
			Result->SetStringField(TEXT("note"), TEXT("Created a PCGGraph asset container because no sourceGraph/source_graph was supplied. Recipe-to-node graph synthesis and scene binding are not implemented in this plugin yet."));
			return JsonObjectToString(Result.ToSharedRef());
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
			AddStep(TEXT("worlddata://mcp/governance"), TEXT("Inspect the enforced tool-risk and approval policy before requesting changes."));
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
