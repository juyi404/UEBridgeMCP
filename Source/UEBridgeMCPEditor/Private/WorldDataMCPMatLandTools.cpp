#include "WorldDataMCPMatLandTools.h"

#include "WorldDataMCPCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "IAssetTools.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace MatLandTools
{
namespace
{
	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

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

	bool SplitDestPath(const FString& Dest, FString& OutFolder, FString& OutName, FString& OutError)
	{
		FString Path = Dest;
		Path.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex)) { Path.LeftInline(DotIndex); }
		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			OutError = TEXT("destPath must be a content path like /Game/Materials/MF_Foo.");
			return false;
		}
		OutName = FPackageName::GetShortName(Path);
		OutFolder = FPackageName::GetLongPackagePath(Path);
		return !OutName.IsEmpty() && !OutFolder.IsEmpty();
	}

	void SaveAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

	UClass* ResolveMaterialExpressionClass(const FString& ExpressionType)
	{
		FString ClassName = ExpressionType;
		if (!ClassName.StartsWith(TEXT("MaterialExpression")) && !ClassName.StartsWith(TEXT("UMaterialExpression")))
		{
			ClassName = TEXT("MaterialExpression") + ClassName;
		}
		auto Validate = [](UClass* C) -> UClass* { return (C && C->IsChildOf(UMaterialExpression::StaticClass())) ? C : nullptr; };
		if (UClass* Direct = Validate(FindObject<UClass>(nullptr, *ClassName))) { return Direct; }
		FString Stripped = ClassName.StartsWith(TEXT("U")) ? ClassName.Mid(1) : ClassName;
		if (UClass* Scripted = Validate(FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Stripped)))) { return Scripted; }
		return nullptr;
	}

	// ---- material functions --------------------------------------------------------------

	FString CreateMaterialFunction(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
		UMaterialFunction* MF = Cast<UMaterialFunction>(AssetTools().CreateAsset(Name, Folder, UMaterialFunction::StaticClass(), Factory));
		if (!MF)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create material function at %s."), *DestPath));
		}
		FString Description;
		if (Args->TryGetStringField(TEXT("description"), Description))
		{
			MF->Description = Description;
		}
		SaveAsset(MF);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MF->GetPathName());
		return SuccessJson(Result);
	}

	FString AddMaterialFunctionExpression(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, ExpressionType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("expressionType"), ExpressionType);
		UMaterialFunction* MF = Cast<UMaterialFunction>(LoadAssetObject(AssetPath));
		if (!MF)
		{
			return ErrorJson(FString::Printf(TEXT("Material function '%s' not found."), *AssetPath));
		}
		UClass* ExprClass = ResolveMaterialExpressionClass(ExpressionType);
		if (!ExprClass)
		{
			return ErrorJson(FString::Printf(TEXT("Material expression type '%s' not found."), *ExpressionType));
		}
		double PosX = 0.0, PosY = 0.0;
		Args->TryGetNumberField(TEXT("posX"), PosX);
		Args->TryGetNumberField(TEXT("posY"), PosY);
		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MF, ExprClass, static_cast<int32>(PosX), static_cast<int32>(PosY));
		if (!Expr)
		{
			return ErrorJson(TEXT("CreateMaterialExpressionInFunction returned null."));
		}

		FString PinName;
		if (Args->TryGetStringField(TEXT("name"), PinName) && !PinName.IsEmpty())
		{
			if (UMaterialExpressionFunctionInput* In = Cast<UMaterialExpressionFunctionInput>(Expr))
			{
				In->InputName = FName(*PinName);
			}
			else if (UMaterialExpressionFunctionOutput* Out = Cast<UMaterialExpressionFunctionOutput>(Expr))
			{
				Out->OutputName = FName(*PinName);
			}
			Expr->Desc = PinName;
		}
		UMaterialEditingLibrary::UpdateMaterialFunction(MF, nullptr);
		SaveAsset(MF);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MF->GetPathName());
		Result->SetStringField(TEXT("expression"), Expr->GetName());
		Result->SetStringField(TEXT("expressionClass"), ExprClass->GetName());
		return SuccessJson(Result);
	}

	UMaterialExpression* FindFunctionExpression(UMaterialFunction* MF, const FString& Name)
	{
		for (UMaterialExpression* Expr : MF->GetExpressions())
		{
			if (!Expr) { continue; }
			if (Expr->Desc == Name || Expr->GetName() == Name) { return Expr; }
			if (UMaterialExpressionFunctionInput* In = Cast<UMaterialExpressionFunctionInput>(Expr))
			{
				if (In->InputName.ToString() == Name) { return Expr; }
			}
			if (UMaterialExpressionFunctionOutput* Out = Cast<UMaterialExpressionFunctionOutput>(Expr))
			{
				if (Out->OutputName.ToString() == Name) { return Expr; }
			}
		}
		return nullptr;
	}

	FString ConnectMaterialFunctionExpression(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, FromName, ToName, FromOutput, ToInput;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("fromExpression"), FromName);
		Args->TryGetStringField(TEXT("toExpression"), ToName);
		Args->TryGetStringField(TEXT("fromOutput"), FromOutput);
		Args->TryGetStringField(TEXT("toInput"), ToInput);
		UMaterialFunction* MF = Cast<UMaterialFunction>(LoadAssetObject(AssetPath));
		if (!MF)
		{
			return ErrorJson(FString::Printf(TEXT("Material function '%s' not found."), *AssetPath));
		}
		UMaterialExpression* From = FindFunctionExpression(MF, FromName);
		UMaterialExpression* To = FindFunctionExpression(MF, ToName);
		if (!From || !To)
		{
			return ErrorJson(TEXT("Source or target expression not found in function."));
		}
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
		{
			return ErrorJson(TEXT("ConnectMaterialExpressions failed (check output/input names)."));
		}
		UMaterialEditingLibrary::UpdateMaterialFunction(MF, nullptr);
		SaveAsset(MF);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MF->GetPathName());
		Result->SetBoolField(TEXT("connected"), true);
		return SuccessJson(Result);
	}

	FString InspectMaterialFunction(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UMaterialFunction* MF = Cast<UMaterialFunction>(LoadAssetObject(AssetPath));
		if (!MF)
		{
			return ErrorJson(FString::Printf(TEXT("Material function '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Exprs;
		int32 Index = 0;
		for (UMaterialExpression* Expr : MF->GetExpressions())
		{
			if (Expr)
			{
				TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetNumberField(TEXT("index"), Index);
				E->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
				E->SetStringField(TEXT("name"), Expr->GetName());
				if (!Expr->Desc.IsEmpty()) { E->SetStringField(TEXT("desc"), Expr->Desc); }
				if (UMaterialExpressionFunctionInput* In = Cast<UMaterialExpressionFunctionInput>(Expr)) { E->SetStringField(TEXT("inputName"), In->InputName.ToString()); }
				if (UMaterialExpressionFunctionOutput* Out = Cast<UMaterialExpressionFunctionOutput>(Expr)) { E->SetStringField(TEXT("outputName"), Out->OutputName.ToString()); }
				Exprs.Add(MakeShared<FJsonValueObject>(E));
			}
			++Index;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), MF->GetPathName());
		Result->SetStringField(TEXT("description"), MF->Description);
		Result->SetNumberField(TEXT("expressionCount"), Exprs.Num());
		Result->SetArrayField(TEXT("expressions"), Exprs);
		return SuccessJson(Result);
	}

	// ---- landscape -----------------------------------------------------------------------

	ALandscapeProxy* FindLandscape(UWorld* World, const FString& Name)
	{
		if (!World) { return nullptr; }
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			ALandscapeProxy* L = *It;
			if (!IsValid(L)) { continue; }
			if (Name.IsEmpty() || L->GetActorLabel() == Name || L->GetName() == Name) { return L; }
		}
		return nullptr;
	}

	FString GetLandscapeInfo(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		TArray<TSharedPtr<FJsonValue>> Landscapes;
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			ALandscapeProxy* L = *It;
			if (!IsValid(L)) { continue; }
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), L->GetActorLabel());
			Entry->SetStringField(TEXT("class"), L->GetClass()->GetName());
			TArray<ULandscapeComponent*> Comps;
			L->GetComponents<ULandscapeComponent>(Comps);
			Entry->SetNumberField(TEXT("componentCount"), Comps.Num());
			Entry->SetStringField(TEXT("material"), L->LandscapeMaterial ? L->LandscapeMaterial->GetPathName() : FString());
			const FBox Bounds = L->GetComponentsBoundingBox();
			if (Bounds.IsValid)
			{
				const FVector Size = Bounds.GetSize();
				Entry->SetNumberField(TEXT("sizeX"), Size.X);
				Entry->SetNumberField(TEXT("sizeY"), Size.Y);
			}
			Landscapes.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), Landscapes.Num());
		Result->SetArrayField(TEXT("landscapes"), Landscapes);
		return SuccessJson(Result);
	}

	FString SetLandscapeMaterial(const TSharedPtr<FJsonObject>& Args)
	{
		FString LandscapeName, MaterialPath;
		Args->TryGetStringField(TEXT("landscape"), LandscapeName);
		Args->TryGetStringField(TEXT("material"), MaterialPath);
		ALandscapeProxy* Landscape = FindLandscape(GetEditorWorld(), LandscapeName);
		if (!Landscape)
		{
			return ErrorJson(FString::Printf(TEXT("Landscape '%s' not found."), *LandscapeName));
		}
		UMaterialInterface* Material = Cast<UMaterialInterface>(LoadAssetObject(MaterialPath));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Material '%s' not found."), *MaterialPath));
		}
		Landscape->Modify();
		Landscape->LandscapeMaterial = Material;
		TArray<ULandscapeComponent*> Comps;
		Landscape->GetComponents<ULandscapeComponent>(Comps);
		for (ULandscapeComponent* Comp : Comps)
		{
			if (Comp)
			{
				Comp->SetMaterial(0, Material);
				Comp->MarkRenderStateDirty();
			}
		}
		Landscape->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
		Result->SetStringField(TEXT("material"), Material->GetPathName());
		Result->SetNumberField(TEXT("componentsUpdated"), Comps.Num());
		return SuccessJson(Result);
	}

	FString CreateLandscapeLayerInfo(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath, LayerName;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		Args->TryGetStringField(TEXT("layerName"), LayerName);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		if (LayerName.IsEmpty()) { LayerName = Name; }
		const FString PackageName = Folder / Name;
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create package '%s'."), *PackageName));
		}
		ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>(Package, *Name, RF_Public | RF_Standalone);
		if (!LayerInfo)
		{
			return ErrorJson(TEXT("Failed to create landscape layer info object."));
		}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		LayerInfo->LayerName = FName(*LayerName);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		FAssetRegistryModule::AssetCreated(LayerInfo);
		SaveAsset(LayerInfo);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), LayerInfo->GetPathName());
		Result->SetStringField(TEXT("layerName"), LayerName);
		return SuccessJson(Result);
	}

	FString AddLandscapeLayerInfo(const TSharedPtr<FJsonObject>& Args)
	{
		FString LandscapeName, LayerInfoPath;
		Args->TryGetStringField(TEXT("landscape"), LandscapeName);
		Args->TryGetStringField(TEXT("layerInfo"), LayerInfoPath);
		ALandscapeProxy* Landscape = FindLandscape(GetEditorWorld(), LandscapeName);
		if (!Landscape)
		{
			return ErrorJson(FString::Printf(TEXT("Landscape '%s' not found."), *LandscapeName));
		}
		ULandscapeLayerInfoObject* LayerInfo = Cast<ULandscapeLayerInfoObject>(LoadAssetObject(LayerInfoPath));
		if (!LayerInfo)
		{
			return ErrorJson(FString::Printf(TEXT("Layer info '%s' not found (create with create_landscape_layer_info)."), *LayerInfoPath));
		}
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info)
		{
			return ErrorJson(TEXT("Landscape has no LandscapeInfo."));
		}
		Landscape->Modify();
		const int32 LayerIndex = Info->Layers.Num();
		Info->Layers.Add(FLandscapeInfoLayerSettings(LayerInfo, Landscape));
		Landscape->MarkPackageDirty();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
		Result->SetStringField(TEXT("layerInfo"), LayerInfo->GetPathName());
		Result->SetNumberField(TEXT("layerIndex"), LayerIndex);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_material_function","description":"Create a UMaterialFunction asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Materials/MF_Foo."},"description":{"type":"string"}},"required":["destPath"]},"annotations":{"title":"Create Material Function","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_material_function_expression","description":"Add an expression node to a material function (e.g. FunctionInput, FunctionOutput, Multiply, Add, ScalarParameter). 'name' sets the Input/Output pin name. Recompiles the function.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"expressionType":{"type":"string"},"name":{"type":"string"},"posX":{"type":"number"},"posY":{"type":"number"}},"required":["assetPath","expressionType"]},"annotations":{"title":"Add Material Function Expression","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"connect_material_function_expression","description":"Connect two expressions inside a material function (by name/Desc/Input-Output name). Wire into a FunctionOutput to expose the function's result.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"fromExpression":{"type":"string"},"toExpression":{"type":"string"},"fromOutput":{"type":"string"},"toInput":{"type":"string"}},"required":["assetPath","fromExpression","toExpression"]},"annotations":{"title":"Connect Material Function Expression","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"inspect_material_function","description":"List a material function's expression nodes (class/name/desc, input/output names).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Material Function","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_landscape_info","description":"List landscapes in the level: component count, assigned material, and bounds.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Landscape Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_landscape_material","description":"Assign a material to a landscape (updates all landscape components).","inputSchema":{"type":"object","properties":{"landscape":{"type":"string","description":"Landscape actor name; empty = first landscape."},"material":{"type":"string"}},"required":["material"]},"annotations":{"title":"Set Landscape Material","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"create_landscape_layer_info","description":"Create a ULandscapeLayerInfoObject asset (paint layer) for use with add_landscape_layer_info.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Landscape/LI_Grass."},"layerName":{"type":"string"}},"required":["destPath"]},"annotations":{"title":"Create Landscape Layer Info","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_landscape_layer_info","description":"Register a landscape layer info (paint layer) on a landscape.","inputSchema":{"type":"object","properties":{"landscape":{"type":"string"},"layerInfo":{"type":"string","description":"LayerInfo asset path."}},"required":["layerInfo"]},"annotations":{"title":"Add Landscape Layer Info","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_material_function")) { OutResult = CreateMaterialFunction(Args); return true; }
	if (ToolName == TEXT("add_material_function_expression")) { OutResult = AddMaterialFunctionExpression(Args); return true; }
	if (ToolName == TEXT("connect_material_function_expression")) { OutResult = ConnectMaterialFunctionExpression(Args); return true; }
	if (ToolName == TEXT("inspect_material_function")) { OutResult = InspectMaterialFunction(Args); return true; }

	if (ToolName == TEXT("get_landscape_info")) { OutResult = GetLandscapeInfo(Args); return true; }
	if (ToolName == TEXT("set_landscape_material")) { OutResult = SetLandscapeMaterial(Args); return true; }
	if (ToolName == TEXT("create_landscape_layer_info")) { OutResult = CreateLandscapeLayerInfo(Args); return true; }
	if (ToolName == TEXT("add_landscape_layer_info")) { OutResult = AddLandscapeLayerInfo(Args); return true; }
	return false;
}
}
}
