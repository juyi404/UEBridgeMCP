#include "WorldDataMCPAuthoringTools.h"

#include "WorldDataMCPCommon.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitterHandle.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/DataTableFactory.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "LevelSequence.h"
#include "Materials/Material.h"
#include "MovieScene.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace WorldDataMCP
{
namespace AuthoringTools
{
namespace
{
	// Split a destination content path like "/Game/Foo/BP_Bar" into folder "/Game/Foo" and
	// asset name "BP_Bar". Accepts an optional object suffix and strips it.
	bool SplitDestPath(const FString& Dest, FString& OutFolder, FString& OutName, FString& OutError)
	{
		FString Path = Dest;
		Path.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex))
		{
			Path.LeftInline(DotIndex);
		}
		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			OutError = TEXT("destPath must be a content path like /Game/Folder/AssetName.");
			return false;
		}
		OutName = FPackageName::GetShortName(Path);
		OutFolder = FPackageName::GetLongPackagePath(Path);
		if (OutName.IsEmpty() || OutFolder.IsEmpty())
		{
			OutError = TEXT("Could not split destPath into folder and asset name.");
			return false;
		}
		return true;
	}

	// Resolve a UClass by short name, object path, or /Script path. Returns nullptr if unknown.
	UClass* ResolveClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Direct = FindObject<UClass>(nullptr, *ClassName))
		{
			return Direct;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName))
		{
			return Loaded;
		}
		// Fall back to scanning by short name (handles bare names like "Actor", "GameplayEffect").
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	FAssetToolsModule& GetAssetToolsModule()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	}

	void SaveCreatedAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

	// Map a friendly type name to a Blueprint/struct pin type. Defaults to bool for unknowns
	// so callers always get a valid, editable member they can re-type in the editor.
	FEdGraphPinType MakePinType(const FString& Type)
	{
		FEdGraphPinType P;
		const FString T = Type.ToLower();
		if (T == TEXT("bool") || T == TEXT("boolean")) { P.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (T == TEXT("byte")) { P.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer")) { P.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (T == TEXT("int64")) { P.PinCategory = UEdGraphSchema_K2::PC_Int64; }
		else if (T == TEXT("float") || T == TEXT("double") || T == TEXT("real"))
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Real;
			P.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (T == TEXT("string")) { P.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (T == TEXT("name")) { P.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (T == TEXT("text")) { P.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else if (T == TEXT("vector"))
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Struct;
			P.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (T == TEXT("rotator"))
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Struct;
			P.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (T == TEXT("transform"))
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Struct;
			P.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		}
		else if (T == TEXT("linearcolor") || T == TEXT("color"))
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Struct;
			P.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		}
		else
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		return P;
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

	// ---- Blueprint + GAS ----------------------------------------------------------------

	FString CreateBlueprintFromParent(const TSharedPtr<FJsonObject>& Args, const FString& DefaultParent, const FString& FriendlyType)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString ParentName = DefaultParent;
		Args->TryGetStringField(TEXT("parentClass"), ParentName);

		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UClass* ParentClass = ResolveClass(ParentName);
		if (!ParentClass)
		{
			return ErrorJson(FString::Printf(TEXT("Parent class '%s' not found."), *ParentName));
		}

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;
		UBlueprint* Blueprint = Cast<UBlueprint>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UBlueprint::StaticClass(), Factory));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create %s at %s."), *FriendlyType, *DestPath));
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		SaveCreatedAsset(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());
		Result->SetStringField(TEXT("type"), FriendlyType);
		return SuccessJson(Result);
	}

	FString CreateBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateBlueprintFromParent(Args, TEXT("Actor"), TEXT("Blueprint"));
	}

	FString CreateGameplayAbility(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateBlueprintFromParent(Args, TEXT("/Script/GameplayAbilities.GameplayAbility"), TEXT("GameplayAbility"));
	}

	FString CreateGameplayEffect(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateBlueprintFromParent(Args, TEXT("/Script/GameplayAbilities.GameplayEffect"), TEXT("GameplayEffect"));
	}

	FString AddBlueprintVariable(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		FString VarName;
		Args->TryGetStringField(TEXT("name"), VarName);
		FString VarType = TEXT("float");
		Args->TryGetStringField(TEXT("type"), VarType);
		if (AssetPath.IsEmpty() || VarName.IsEmpty())
		{
			return ErrorJson(TEXT("Both 'assetPath' and 'name' are required."));
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		const bool bOk = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), MakePinType(VarType));
		if (!bOk)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to add variable '%s' (name may already exist)."), *VarName));
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		SaveCreatedAsset(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("variable"), VarName);
		Result->SetStringField(TEXT("type"), VarType);
		return SuccessJson(Result);
	}

	FString CompileBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetNumberField(TEXT("errors"), Results.NumErrors);
		Result->SetNumberField(TEXT("warnings"), Results.NumWarnings);
		return SuccessJson(Result);
	}

	FString ListBlueprintsByParent(const FString& ParentSubstring)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Assets;
		ARM.Get().GetAssets(Filter, Assets);

		TArray<TSharedPtr<FJsonValue>> Matches;
		for (const FAssetData& Asset : Assets)
		{
			FString ParentClass;
			if (!Asset.GetTagValue(TEXT("ParentClass"), ParentClass))
			{
				Asset.GetTagValue(TEXT("NativeParentClass"), ParentClass);
			}
			if (ParentClass.Contains(ParentSubstring))
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("asset"), Asset.GetObjectPathString());
				Entry->SetStringField(TEXT("parentClass"), ParentClass);
				Matches.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), Matches.Num());
		Result->SetArrayField(TEXT("assets"), Matches);
		return SuccessJson(Result);
	}

	FString ListGameplayAbilities(const TSharedPtr<FJsonObject>& Args)
	{
		return ListBlueprintsByParent(TEXT("GameplayAbility"));
	}

	FString ListGameplayEffects(const TSharedPtr<FJsonObject>& Args)
	{
		return ListBlueprintsByParent(TEXT("GameplayEffect"));
	}

	// ---- Material + Niagara -------------------------------------------------------------

	FString CreateMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UMaterial* Material = Cast<UMaterial>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UMaterial::StaticClass(), Factory));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create material at %s."), *DestPath));
		}
		SaveCreatedAsset(Material);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
		return SuccessJson(Result);
	}

	FString CreateNiagaraSystem(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		// The Niagara system factory lives in the NiagaraEditor module; load it dynamically by
		// class path so this module doesn't need a compile-time dependency on the factory header.
		UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"));
		UFactory* Factory = FactoryClass ? Cast<UFactory>(NewObject<UObject>(GetTransientPackage(), FactoryClass)) : nullptr;
		if (!Factory)
		{
			return ErrorJson(TEXT("NiagaraSystemFactoryNew not available (is the Niagara plugin enabled?)."));
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UNiagaraSystem::StaticClass(), Factory));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create Niagara system at %s."), *DestPath));
		}
		SaveCreatedAsset(System);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		return SuccessJson(Result);
	}

	FString ReadNiagaraSystem(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(AssetPath));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Niagara system '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Emitters;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Handle.GetName().ToString());
			Entry->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
			Emitters.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		Result->SetNumberField(TEXT("emitterCount"), Emitters.Num());
		Result->SetArrayField(TEXT("emitters"), Emitters);
		return SuccessJson(Result);
	}

	// ---- Sequencer + Widget -------------------------------------------------------------

	FString CreateSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		const FString PackageName = Folder / Name;
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create package '%s' (invalid destPath?)."), *PackageName));
		}
		ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Name, RF_Public | RF_Standalone);
		if (!Sequence)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create level sequence at %s."), *DestPath));
		}
		Sequence->Initialize();
		FAssetRegistryModule::AssetCreated(Sequence);
		SaveCreatedAsset(Sequence);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Sequence->GetPathName());
		return SuccessJson(Result);
	}

	FString BindActorToSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		FString ActorName;
		Args->TryGetStringField(TEXT("actor"), ActorName);
		ULevelSequence* Sequence = Cast<ULevelSequence>(LoadAssetObject(AssetPath));
		if (!Sequence)
		{
			return ErrorJson(FString::Printf(TEXT("Level sequence '%s' not found."), *AssetPath));
		}
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AActor* Actor = nullptr;
		if (World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsValid(*It) && ((*It)->GetActorLabel() == ActorName || (*It)->GetName() == ActorName))
				{
					Actor = *It;
					break;
				}
			}
		}
		if (!Actor)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found in the editor world."), *ActorName));
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			return ErrorJson(TEXT("Sequence has no MovieScene."));
		}
		const FGuid Guid = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
		Sequence->BindPossessableObject(Guid, *Actor, Actor->GetWorld());
		SaveCreatedAsset(Sequence);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Sequence->GetPathName());
		Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
		Result->SetStringField(TEXT("bindingId"), Guid.ToString());
		return SuccessJson(Result);
	}

	FString ReadSequence(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		ULevelSequence* Sequence = Cast<ULevelSequence>(LoadAssetObject(AssetPath));
		if (!Sequence)
		{
			return ErrorJson(FString::Printf(TEXT("Level sequence '%s' not found."), *AssetPath));
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Sequence->GetPathName());
		if (MovieScene)
		{
			Result->SetNumberField(TEXT("possessableCount"), MovieScene->GetPossessableCount());
			Result->SetNumberField(TEXT("spawnableCount"), MovieScene->GetSpawnableCount());
			Result->SetNumberField(TEXT("trackCount"), MovieScene->GetTracks().Num());
		}
		return SuccessJson(Result);
	}

	FString CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = UUserWidget::StaticClass();
		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UWidgetBlueprint::StaticClass(), Factory));
		if (!WidgetBP)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create widget blueprint at %s."), *DestPath));
		}
		SaveCreatedAsset(WidgetBP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), WidgetBP->GetPathName());
		return SuccessJson(Result);
	}

	FString AddWidget(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		FString WidgetClassName;
		Args->TryGetStringField(TEXT("widgetClass"), WidgetClassName);
		FString WidgetName;
		Args->TryGetStringField(TEXT("name"), WidgetName);
		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadAssetObject(AssetPath));
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return ErrorJson(FString::Printf(TEXT("Widget blueprint '%s' not found."), *AssetPath));
		}
		UClass* WidgetClass = ResolveClass(WidgetClassName);
		if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			return ErrorJson(FString::Printf(TEXT("'%s' is not a UWidget subclass."), *WidgetClassName));
		}
		UWidget* NewWidget = WidgetBP->WidgetTree->ConstructWidget<UWidget>(
			WidgetClass, WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName));
		if (!NewWidget)
		{
			return ErrorJson(TEXT("Failed to construct widget."));
		}
		if (WidgetBP->WidgetTree->RootWidget == nullptr)
		{
			WidgetBP->WidgetTree->RootWidget = NewWidget;
		}
		else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WidgetBP->WidgetTree->RootWidget))
		{
			RootPanel->AddChild(NewWidget);
		}
		else
		{
			return ErrorJson(TEXT("Root widget is not a panel; cannot add a child. Set a panel root first."));
		}
		FKismetEditorUtilities::CompileBlueprint(WidgetBP);
		SaveCreatedAsset(WidgetBP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), WidgetBP->GetPathName());
		Result->SetStringField(TEXT("widget"), NewWidget->GetName());
		Result->SetStringField(TEXT("widgetClass"), WidgetClass->GetName());
		return SuccessJson(Result);
	}

	FString ReadWidgetBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadAssetObject(AssetPath));
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return ErrorJson(FString::Printf(TEXT("Widget blueprint '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Widgets;
		WidgetBP->WidgetTree->ForEachWidget([&Widgets](UWidget* Widget)
		{
			if (Widget)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Widget->GetName());
				Entry->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
				Widgets.Add(MakeShared<FJsonValueObject>(Entry));
			}
		});
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), WidgetBP->GetPathName());
		Result->SetStringField(TEXT("rootWidget"),
			WidgetBP->WidgetTree->RootWidget ? WidgetBP->WidgetTree->RootWidget->GetName() : FString(TEXT("(none)")));
		Result->SetNumberField(TEXT("widgetCount"), Widgets.Num());
		Result->SetArrayField(TEXT("widgets"), Widgets);
		return SuccessJson(Result);
	}

	// ---- Data tables / structs / enums --------------------------------------------------

	UScriptStruct* ResolveScriptStruct(const FString& StructName)
	{
		if (UScriptStruct* Direct = LoadObject<UScriptStruct>(nullptr, *StructName))
		{
			return Direct;
		}
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->GetName() == StructName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString CreateDataTable(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString RowStructName;
		Args->TryGetStringField(TEXT("rowStruct"), RowStructName);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UScriptStruct* RowStruct = ResolveScriptStruct(RowStructName);
		if (!RowStruct)
		{
			return ErrorJson(FString::Printf(TEXT("Row struct '%s' not found."), *RowStructName));
		}
		UDataTableFactory* Factory = NewObject<UDataTableFactory>();
		Factory->Struct = RowStruct;
		UDataTable* DataTable = Cast<UDataTable>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UDataTable::StaticClass(), Factory));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create data table at %s."), *DestPath));
		}
		SaveCreatedAsset(DataTable);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), DataTable->GetPathName());
		Result->SetStringField(TEXT("rowStruct"), RowStruct->GetName());
		return SuccessJson(Result);
	}

	FString AddDataTableRow(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		FString RowName;
		Args->TryGetStringField(TEXT("rowName"), RowName);
		const TSharedPtr<FJsonObject>* RowObj = nullptr;
		if (AssetPath.IsEmpty() || RowName.IsEmpty() || !Args->TryGetObjectField(TEXT("row"), RowObj) || !RowObj)
		{
			return ErrorJson(TEXT("'assetPath', 'rowName', and 'row' (object) are required."));
		}
		UDataTable* DataTable = Cast<UDataTable>(LoadAssetObject(AssetPath));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("Data table '%s' not found."), *AssetPath));
		}
		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		if (!RowStruct)
		{
			return ErrorJson(TEXT("Data table has no row struct."));
		}
		// Build a transient row buffer of the table's row type from the JSON object.
		void* RowData = FMemory::Malloc(FMath::Max<int32>(1, RowStruct->GetStructureSize()));
		RowStruct->InitializeStruct(RowData);
		FJsonObjectConverter::JsonObjectToUStruct(RowObj->ToSharedRef(), RowStruct, RowData, 0, 0);
		DataTable->AddRow(FName(*RowName), *reinterpret_cast<FTableRowBase*>(RowData));
		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);
		SaveCreatedAsset(DataTable);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), DataTable->GetPathName());
		Result->SetStringField(TEXT("rowName"), RowName);
		Result->SetNumberField(TEXT("rowCount"), DataTable->GetRowMap().Num());
		return SuccessJson(Result);
	}

	FString ReadDataTable(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UDataTable* DataTable = Cast<UDataTable>(LoadAssetObject(AssetPath));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("Data table '%s' not found."), *AssetPath));
		}
		const UScriptStruct* RowStruct = DataTable->GetRowStruct();

		double MaxRowsNumber = 100.0;
		Args->TryGetNumberField(TEXT("maxRows"), MaxRowsNumber);
		const int32 MaxRows = FMath::Clamp(static_cast<int32>(MaxRowsNumber), 1, 1000);

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TPair<FName, uint8*>& Pair : DataTable->GetRowMap())
		{
			if (Rows.Num() >= MaxRows)
			{
				break;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("rowName"), Pair.Key.ToString());
			if (RowStruct)
			{
				TSharedRef<FJsonObject> RowJson = MakeShared<FJsonObject>();
				FJsonObjectConverter::UStructToJsonObject(RowStruct, Pair.Value, RowJson, 0, 0);
				Entry->SetObjectField(TEXT("data"), RowJson);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), DataTable->GetPathName());
		Result->SetStringField(TEXT("rowStruct"), RowStruct ? RowStruct->GetName() : FString());
		Result->SetNumberField(TEXT("rowCount"), DataTable->GetRowMap().Num());
		Result->SetArrayField(TEXT("rows"), Rows);
		return SuccessJson(Result);
	}

	FString CreateEnum(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.EnumFactory"));
		UFactory* Factory = FactoryClass ? Cast<UFactory>(NewObject<UObject>(GetTransientPackage(), FactoryClass)) : nullptr;
		if (!Factory)
		{
			return ErrorJson(TEXT("EnumFactory not available."));
		}
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(
			GetAssetToolsModule().Get().CreateAsset(Name, Folder, UUserDefinedEnum::StaticClass(), Factory));
		if (!Enum)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create enum at %s."), *DestPath));
		}

		// Optionally seed entries.
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Args->TryGetArrayField(TEXT("entries"), Entries) && Entries)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
				const int32 NewIndex = Enum->NumEnums() - 2; // -1 entry is the implicit _MAX
				if (NewIndex >= 0)
				{
					FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(EntryValue->AsString()));
				}
			}
		}
		SaveCreatedAsset(Enum);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Enum->GetPathName());
		Result->SetNumberField(TEXT("entryCount"), FMath::Max(0, Enum->NumEnums() - 1));
		return SuccessJson(Result);
	}

	FString CreateStruct(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		const FString PackageName = Folder / Name;
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create package '%s' (invalid destPath?)."), *PackageName));
		}
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone);
		if (!Struct)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create struct at %s."), *DestPath));
		}

		// Add requested typed fields (names auto-assigned by the editor; re-name in-editor).
		int32 AddedFields = 0;
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (Args->TryGetArrayField(TEXT("fields"), Fields) && Fields)
		{
			for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
			{
				FString TypeName = TEXT("float");
				const TSharedPtr<FJsonObject>* FieldObj = nullptr;
				if (FieldValue->TryGetObject(FieldObj) && FieldObj)
				{
					(*FieldObj)->TryGetStringField(TEXT("type"), TypeName);
				}
				else
				{
					TypeName = FieldValue->AsString();
				}
				if (FStructureEditorUtils::AddVariable(Struct, MakePinType(TypeName)))
				{
					++AddedFields;
				}
			}
		}
		FAssetRegistryModule::AssetCreated(Struct);
		SaveCreatedAsset(Struct);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Struct->GetPathName());
		Result->SetNumberField(TEXT("addedFields"), AddedFields);
		Result->SetStringField(TEXT("note"), TEXT("Struct starts with one default member; field names are auto-assigned and editable in-editor."));
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_blueprint","description":"Create a Blueprint asset deriving from a parent class (default Actor). Compiles and saves it.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Blueprints/BP_Foo."},"parentClass":{"type":"string","description":"Parent class name or /Script path. Default Actor."}},"required":["destPath"]},"annotations":{"title":"Create Blueprint","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_variable","description":"Add a member variable of a given type to a Blueprint, then compile and save. Types: bool, byte, int, int64, float, string, name, text, vector, rotator, transform, color.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"},"type":{"type":"string"}},"required":["assetPath","name"]},"annotations":{"title":"Add Blueprint Variable","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"compile_blueprint","description":"Compile a Blueprint and return the error/warning counts.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Compile Blueprint","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"create_gameplay_ability","description":"Create a GameplayAbility Blueprint (parent UGameplayAbility, or a custom parentClass). Requires the GameplayAbilities plugin.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"},"parentClass":{"type":"string"}},"required":["destPath"]},"annotations":{"title":"Create Gameplay Ability","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_gameplay_effect","description":"Create a GameplayEffect Blueprint (parent UGameplayEffect, or a custom parentClass).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"},"parentClass":{"type":"string"}},"required":["destPath"]},"annotations":{"title":"Create Gameplay Effect","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"list_gameplay_abilities","description":"List Blueprint assets whose parent class is a GameplayAbility.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Gameplay Abilities","readOnlyHint":true,"openWorldHint":false}},
{"name":"list_gameplay_effects","description":"List Blueprint assets whose parent class is a GameplayEffect.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List Gameplay Effects","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_material","description":"Create an empty Material asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Materials/M_Foo."}},"required":["destPath"]},"annotations":{"title":"Create Material","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_niagara_system","description":"Create an empty Niagara system asset. Requires the Niagara plugin.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"}},"required":["destPath"]},"annotations":{"title":"Create Niagara System","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_niagara_system","description":"Read a Niagara system's emitter handles (name and enabled state).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Niagara System","readOnlyHint":true,"openWorldHint":false}},
)JSON")
		TEXT(R"JSON({"name":"create_sequence","description":"Create a Level Sequence asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Cinematics/LS_Foo."}},"required":["destPath"]},"annotations":{"title":"Create Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"bind_actor_to_sequence","description":"Add a level actor as a possessable binding in a Level Sequence.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"actor":{"type":"string","description":"Actor name or label in the current editor world."}},"required":["assetPath","actor"]},"annotations":{"title":"Bind Actor To Sequence","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_sequence","description":"Read a Level Sequence: possessable, spawnable, and track counts.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Sequence","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_widget_blueprint","description":"Create a UMG Widget Blueprint (parent UserWidget).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/UI/WBP_Foo."}},"required":["destPath"]},"annotations":{"title":"Create Widget Blueprint","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_widget","description":"Construct a widget (by class, e.g. Button, TextBlock, VerticalBox) into a Widget Blueprint's tree. Sets it as root if empty, else adds it to the root panel.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"widgetClass":{"type":"string"},"name":{"type":"string"}},"required":["assetPath","widgetClass"]},"annotations":{"title":"Add Widget","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_widget_blueprint","description":"Read a Widget Blueprint's tree: root widget and the name/class of every widget.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Widget Blueprint","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_data_table","description":"Create a Data Table asset with the given row struct (UserDefinedStruct path or a native FTableRowBase struct name).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"},"rowStruct":{"type":"string"}},"required":["destPath","rowStruct"]},"annotations":{"title":"Create Data Table","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_data_table_row","description":"Add a row to a Data Table. 'row' is a JSON object matching the table's row struct fields.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"rowName":{"type":"string"},"row":{"type":"object"}},"required":["assetPath","rowName","row"]},"annotations":{"title":"Add Data Table Row","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_data_table","description":"Read a Data Table's rows as JSON (each row exported from its struct).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"maxRows":{"type":"number","description":"Default 100, capped at 1000."}},"required":["assetPath"]},"annotations":{"title":"Read Data Table","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_enum","description":"Create a UserDefinedEnum. Optionally seed 'entries' (array of display-name strings).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"},"entries":{"type":"array","items":{"type":"string"}}},"required":["destPath"]},"annotations":{"title":"Create Enum","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_struct","description":"Create a UserDefinedStruct. Optionally add 'fields' (array of {type} or type strings: bool, int, float, string, name, vector, rotator, transform, color). Field names are auto-assigned.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string"},"fields":{"type":"array"}},"required":["destPath"]},"annotations":{"title":"Create Struct","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_blueprint")) { OutResult = CreateBlueprint(Args); return true; }
	if (ToolName == TEXT("add_blueprint_variable")) { OutResult = AddBlueprintVariable(Args); return true; }
	if (ToolName == TEXT("compile_blueprint")) { OutResult = CompileBlueprint(Args); return true; }
	if (ToolName == TEXT("create_gameplay_ability")) { OutResult = CreateGameplayAbility(Args); return true; }
	if (ToolName == TEXT("create_gameplay_effect")) { OutResult = CreateGameplayEffect(Args); return true; }
	if (ToolName == TEXT("list_gameplay_abilities")) { OutResult = ListGameplayAbilities(Args); return true; }
	if (ToolName == TEXT("list_gameplay_effects")) { OutResult = ListGameplayEffects(Args); return true; }

	if (ToolName == TEXT("create_material")) { OutResult = CreateMaterial(Args); return true; }
	if (ToolName == TEXT("create_niagara_system")) { OutResult = CreateNiagaraSystem(Args); return true; }
	if (ToolName == TEXT("read_niagara_system")) { OutResult = ReadNiagaraSystem(Args); return true; }

	if (ToolName == TEXT("create_sequence")) { OutResult = CreateSequence(Args); return true; }
	if (ToolName == TEXT("bind_actor_to_sequence")) { OutResult = BindActorToSequence(Args); return true; }
	if (ToolName == TEXT("read_sequence")) { OutResult = ReadSequence(Args); return true; }
	if (ToolName == TEXT("create_widget_blueprint")) { OutResult = CreateWidgetBlueprint(Args); return true; }
	if (ToolName == TEXT("add_widget")) { OutResult = AddWidget(Args); return true; }
	if (ToolName == TEXT("read_widget_blueprint")) { OutResult = ReadWidgetBlueprint(Args); return true; }

	if (ToolName == TEXT("create_data_table")) { OutResult = CreateDataTable(Args); return true; }
	if (ToolName == TEXT("add_data_table_row")) { OutResult = AddDataTableRow(Args); return true; }
	if (ToolName == TEXT("read_data_table")) { OutResult = ReadDataTable(Args); return true; }
	if (ToolName == TEXT("create_enum")) { OutResult = CreateEnum(Args); return true; }
	if (ToolName == TEXT("create_struct")) { OutResult = CreateStruct(Args); return true; }

	return false;
}
}
}
