#include "WorldDataMCPGameplayTools.h"

#include "WorldDataMCPCommon.h"

#include "AI/NavigationSystemBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace WorldDataMCP
{
namespace GameplayTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : GWorld;
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

	FVector ReadVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FVector& Fallback)
	{
		const TSharedPtr<FJsonObject>* VObj = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, VObj) && VObj && (*VObj).IsValid())
		{
			double X = Fallback.X, Y = Fallback.Y, Z = Fallback.Z;
			(*VObj)->TryGetNumberField(TEXT("x"), X);
			(*VObj)->TryGetNumberField(TEXT("y"), Y);
			(*VObj)->TryGetNumberField(TEXT("z"), Z);
			return FVector(X, Y, Z);
		}
		return Fallback;
	}

	TSharedRef<FJsonObject> VecToJson(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	UNavigationSystemV1* GetNavSys(FString& OutError)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			OutError = TEXT("No editor world available.");
			return nullptr;
		}
		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!NavSys)
		{
			OutError = TEXT("No navigation system in the current world.");
		}
		return NavSys;
	}

	// ---- navigation ----------------------------------------------------------------------

	FString ProjectPointToNavigation(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		UNavigationSystemV1* NavSys = GetNavSys(Error);
		if (!NavSys)
		{
			return ErrorJson(Error);
		}
		const FVector Point = ReadVec(Args, TEXT("location"), FVector::ZeroVector);
		FVector Extent = ReadVec(Args, TEXT("extent"), FVector::ZeroVector);

		FNavLocation NavLocation;
		bool bProjected = Extent.IsZero()
			? NavSys->ProjectPointToNavigation(Point, NavLocation)
			: NavSys->ProjectPointToNavigation(Point, NavLocation, Extent);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("projected"), bProjected);
		if (bProjected)
		{
			Result->SetObjectField(TEXT("projectedLocation"), VecToJson(NavLocation.Location));
		}
		return SuccessJson(Result);
	}

	FString FindNavPath(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		UNavigationSystemV1* NavSys = GetNavSys(Error);
		if (!NavSys)
		{
			return ErrorJson(Error);
		}
		const FVector Start = ReadVec(Args, TEXT("start"), FVector::ZeroVector);
		const FVector End = ReadVec(Args, TEXT("end"), FVector::ZeroVector);

		UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetEditorWorld(), Start, End);
		const bool bValid = Path && Path->IsValid();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("pathFound"), bValid);
		if (Path)
		{
			Result->SetNumberField(TEXT("length"), Path->GetPathLength());
			TArray<TSharedPtr<FJsonValue>> Points;
			for (const FVector& P : Path->PathPoints)
			{
				Points.Add(MakeShared<FJsonValueObject>(VecToJson(P)));
			}
			Result->SetArrayField(TEXT("points"), Points);
			Result->SetNumberField(TEXT("pointCount"), Points.Num());
		}
		return SuccessJson(Result);
	}

	FString RebuildNavigation(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		UNavigationSystemV1* NavSys = GetNavSys(Error);
		if (!NavSys)
		{
			return ErrorJson(Error);
		}
		NavSys->Build();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("rebuilt"), true);
		Result->SetNumberField(TEXT("navDataCount"), NavSys->NavDataSet.Num());
		return SuccessJson(Result);
	}

	FString GetNavmeshInfo(const TSharedPtr<FJsonObject>& Args)
	{
		FString Error;
		UNavigationSystemV1* NavSys = GetNavSys(Error);
		if (!NavSys)
		{
			return ErrorJson(Error);
		}
		ARecastNavMesh* Recast = nullptr;
		for (ANavigationData* NavData : NavSys->NavDataSet)
		{
			Recast = Cast<ARecastNavMesh>(NavData);
			if (Recast) { break; }
		}
		if (!Recast)
		{
			for (TActorIterator<ARecastNavMesh> It(GetEditorWorld()); It; ++It)
			{
				Recast = *It;
				break;
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("hasNavMesh"), Recast != nullptr);
		Result->SetNumberField(TEXT("navDataCount"), NavSys->NavDataSet.Num());
		if (Recast)
		{
			Result->SetStringField(TEXT("recastName"), Recast->GetName());
			Result->SetNumberField(TEXT("tileSizeUU"), Recast->TileSizeUU);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			Result->SetNumberField(TEXT("cellSize"), Recast->GetCellSize(ENavigationDataResolution::Default));
#else
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			Result->SetNumberField(TEXT("cellSize"), Recast->CellSize);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
			Result->SetNumberField(TEXT("agentRadius"), Recast->AgentRadius);
			Result->SetNumberField(TEXT("agentHeight"), Recast->AgentHeight);
		}
		return SuccessJson(Result);
	}

	// ---- enhanced input ------------------------------------------------------------------

	UObject* CreateAsset(UClass* Class, const FString& Name, const FString& PackagePath, FString& OutError)
	{
		const FString PackageFullPath = PackagePath / Name;
		if (UEditorAssetLibrary::DoesAssetExist(PackageFullPath))
		{
			OutError = FString::Printf(TEXT("Asset already exists: %s"), *PackageFullPath);
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackageFullPath);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageFullPath);
			return nullptr;
		}
		UObject* Asset = NewObject<UObject>(Package, Class, *Name, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("NewObject failed.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	FString CreateInputAction(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		FString PackagePath = TEXT("/Game/Input");
		Args->TryGetStringField(TEXT("packagePath"), PackagePath);

		FString Error;
		UInputAction* Action = Cast<UInputAction>(CreateAsset(UInputAction::StaticClass(), Name, PackagePath, Error));
		if (!Action)
		{
			return ErrorJson(Error);
		}
		FString ValueType = TEXT("Boolean");
		Args->TryGetStringField(TEXT("valueType"), ValueType);
		if (ValueType.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || ValueType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
		{
			Action->ValueType = EInputActionValueType::Axis1D;
		}
		else if (ValueType.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || ValueType.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
		{
			Action->ValueType = EInputActionValueType::Axis2D;
		}
		else if (ValueType.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || ValueType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
		{
			Action->ValueType = EInputActionValueType::Axis3D;
		}
		else
		{
			Action->ValueType = EInputActionValueType::Boolean;
		}
		UEditorAssetLibrary::SaveLoadedAsset(Action, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Action->GetPathName());
		Result->SetStringField(TEXT("name"), Action->GetName());
		Result->SetStringField(TEXT("valueType"), ValueType);
		return SuccessJson(Result);
	}

	FString CreateInputMappingContext(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name;
		Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'name'."));
		}
		FString PackagePath = TEXT("/Game/Input");
		Args->TryGetStringField(TEXT("packagePath"), PackagePath);

		FString Error;
		UInputMappingContext* IMC = Cast<UInputMappingContext>(CreateAsset(UInputMappingContext::StaticClass(), Name, PackagePath, Error));
		if (!IMC)
		{
			return ErrorJson(Error);
		}
		UEditorAssetLibrary::SaveLoadedAsset(IMC, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), IMC->GetPathName());
		Result->SetStringField(TEXT("name"), IMC->GetName());
		return SuccessJson(Result);
	}

	FString AddInputMapping(const TSharedPtr<FJsonObject>& Args)
	{
		FString ImcPath, ActionPath, KeyName;
		Args->TryGetStringField(TEXT("mappingContextPath"), ImcPath);
		Args->TryGetStringField(TEXT("inputActionPath"), ActionPath);
		Args->TryGetStringField(TEXT("key"), KeyName);

		UInputMappingContext* IMC = Cast<UInputMappingContext>(LoadAssetObject(ImcPath));
		if (!IMC)
		{
			return ErrorJson(FString::Printf(TEXT("InputMappingContext '%s' not found."), *ImcPath));
		}
		UInputAction* Action = Cast<UInputAction>(LoadAssetObject(ActionPath));
		if (!Action)
		{
			return ErrorJson(FString::Printf(TEXT("InputAction '%s' not found."), *ActionPath));
		}
		const FKey Key(*KeyName);
		if (!Key.IsValid())
		{
			return ErrorJson(FString::Printf(TEXT("Invalid key: '%s'."), *KeyName));
		}
		// Idempotency: skip if this action+key pair already mapped.
		for (const FEnhancedActionKeyMapping& M : IMC->GetMappings())
		{
			if (M.Action == Action && M.Key == Key)
			{
				return ErrorJson(TEXT("This action+key mapping already exists."));
			}
		}
		IMC->MapKey(Action, Key);
		IMC->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(IMC, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("mappingContext"), IMC->GetPathName());
		Result->SetStringField(TEXT("inputAction"), Action->GetPathName());
		Result->SetStringField(TEXT("key"), KeyName);
		Result->SetNumberField(TEXT("mappingCount"), IMC->GetMappings().Num());
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"project_point_to_navigation","description":"Project a world point onto the navmesh. Optional 'extent' {x,y,z} search box.","inputSchema":{"type":"object","properties":{"location":{"type":"object"},"extent":{"type":"object"}},"required":["location"]},"annotations":{"title":"Project Point To Navigation","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_nav_path","description":"Find a navmesh path between two world points (synchronous). Returns path points, length, and whether a valid path was found.","inputSchema":{"type":"object","properties":{"start":{"type":"object"},"end":{"type":"object"}},"required":["start","end"]},"annotations":{"title":"Find Nav Path","readOnlyHint":true,"openWorldHint":false}},
{"name":"rebuild_navigation","description":"Rebuild all navigation data in the current editor world (UNavigationSystemV1::Build).","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Rebuild Navigation","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"get_navmesh_info","description":"Report whether a RecastNavMesh exists in the world plus its key build params (tile size, cell size, agent radius/height).","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Navmesh Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_input_action","description":"Create an Enhanced Input UInputAction asset. valueType: Boolean (default), Axis1D/Float, Axis2D/Vector2D, Axis3D/Vector.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"packagePath":{"type":"string","description":"Default /Game/Input."},"valueType":{"type":"string"}},"required":["name"]},"annotations":{"title":"Create Input Action","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_input_mapping_context","description":"Create an Enhanced Input UInputMappingContext asset.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"packagePath":{"type":"string","description":"Default /Game/Input."}},"required":["name"]},"annotations":{"title":"Create Input Mapping Context","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_input_mapping","description":"Map a key to an input action inside a mapping context (IMC->MapKey). 'key' is an FKey name (e.g. SpaceBar, W, Gamepad_FaceButton_Bottom, LeftMouseButton).","inputSchema":{"type":"object","properties":{"mappingContextPath":{"type":"string"},"inputActionPath":{"type":"string"},"key":{"type":"string"}},"required":["mappingContextPath","inputActionPath","key"]},"annotations":{"title":"Add Input Mapping","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("project_point_to_navigation")) { OutResult = ProjectPointToNavigation(Args); return true; }
	if (ToolName == TEXT("find_nav_path")) { OutResult = FindNavPath(Args); return true; }
	if (ToolName == TEXT("rebuild_navigation")) { OutResult = RebuildNavigation(Args); return true; }
	if (ToolName == TEXT("get_navmesh_info")) { OutResult = GetNavmeshInfo(Args); return true; }
	if (ToolName == TEXT("create_input_action")) { OutResult = CreateInputAction(Args); return true; }
	if (ToolName == TEXT("create_input_mapping_context")) { OutResult = CreateInputMappingContext(Args); return true; }
	if (ToolName == TEXT("add_input_mapping")) { OutResult = AddInputMapping(Args); return true; }
	return false;
}
}
}
