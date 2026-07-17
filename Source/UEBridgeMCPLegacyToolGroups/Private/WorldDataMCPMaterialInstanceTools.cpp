#include "WorldDataMCPMaterialInstanceTools.h"

#include "WorldDataMCPCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace WorldDataMCP
{
namespace MaterialInstanceTools
{
namespace
{
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

	UMaterialInstanceConstant* LoadMIC(const FString& Path, FString& OutError)
	{
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(LoadAssetObject(Path));
		if (!MIC)
		{
			OutError = FString::Printf(TEXT("Material instance constant '%s' not found."), *Path);
		}
		return MIC;
	}

	void Finalize(UMaterialInstanceConstant* MIC)
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
		MIC->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(MIC, /*bOnlyIfIsDirty*/false);
	}

	void AppendNames(const TCHAR* Key, const TArray<FName>& Names, TSharedRef<FJsonObject>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FName& N : Names) { Arr.Add(MakeShared<FJsonValueString>(N.ToString())); }
		Out->SetArrayField(Key, Arr);
	}

	// ---- tools ---------------------------------------------------------------------------

	// NOTE: create_material_instance is intentionally NOT defined here — the older
	// WorldDataMCPEditorTools already provides it (parentPath/destPath schema). This module
	// only adds the per-parameter setters + the read-only parameter lister.

	FString SetScalar(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Param;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("parameter"), Param);
		double Value = 0.0;
		Args->TryGetNumberField(TEXT("value"), Value);
		FString Error;
		UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Error);
		if (!MIC) { return ErrorJson(Error); }

		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, FName(*Param), static_cast<float>(Value));
		Finalize(MIC);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), MIC->GetPathName());
		Result->SetStringField(TEXT("parameter"), Param);
		Result->SetNumberField(TEXT("value"), Value);
		Result->SetBoolField(TEXT("parameterExists"), bOk);
		return SuccessJson(Result);
	}

	FString SetVector(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Param;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("parameter"), Param);
		FString Error;
		UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Error);
		if (!MIC) { return ErrorJson(Error); }

		double R = 0, G = 0, B = 0, A = 1;
		const TSharedPtr<FJsonObject>* VObj = nullptr;
		if (Args->TryGetObjectField(TEXT("value"), VObj) && VObj && (*VObj).IsValid())
		{
			(*VObj)->TryGetNumberField(TEXT("r"), R);
			(*VObj)->TryGetNumberField(TEXT("g"), G);
			(*VObj)->TryGetNumberField(TEXT("b"), B);
			(*VObj)->TryGetNumberField(TEXT("a"), A);
		}
		const FLinearColor Color(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, FName(*Param), Color);
		Finalize(MIC);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), MIC->GetPathName());
		Result->SetStringField(TEXT("parameter"), Param);
		Result->SetBoolField(TEXT("parameterExists"), bOk);
		return SuccessJson(Result);
	}

	FString SetTexture(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Param, TexturePath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("parameter"), Param);
		Args->TryGetStringField(TEXT("texture"), TexturePath);
		FString Error;
		UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Error);
		if (!MIC) { return ErrorJson(Error); }
		UTexture* Texture = Cast<UTexture>(LoadAssetObject(TexturePath));
		if (!Texture)
		{
			return ErrorJson(FString::Printf(TEXT("Texture '%s' not found."), *TexturePath));
		}
		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, FName(*Param), Texture);
		Finalize(MIC);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), MIC->GetPathName());
		Result->SetStringField(TEXT("parameter"), Param);
		Result->SetStringField(TEXT("texture"), Texture->GetPathName());
		Result->SetBoolField(TEXT("parameterExists"), bOk);
		return SuccessJson(Result);
	}

	FString SetSwitch(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Param;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("parameter"), Param);
		bool Value = false;
		Args->TryGetBoolField(TEXT("value"), Value);
		FString Error;
		UMaterialInstanceConstant* MIC = LoadMIC(AssetPath, Error);
		if (!MIC) { return ErrorJson(Error); }

		const bool bOk = UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, FName(*Param), Value);
		Finalize(MIC);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), MIC->GetPathName());
		Result->SetStringField(TEXT("parameter"), Param);
		Result->SetBoolField(TEXT("value"), Value);
		Result->SetBoolField(TEXT("parameterExists"), bOk);
		return SuccessJson(Result);
	}

	FString ListParameters(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UMaterialInterface* Material = Cast<UMaterialInterface>(LoadAssetObject(AssetPath));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Material / material instance '%s' not found."), *AssetPath));
		}
		TArray<FName> Scalars, Vectors, Textures, Switches;
		UMaterialEditingLibrary::GetScalarParameterNames(Material, Scalars);
		UMaterialEditingLibrary::GetVectorParameterNames(Material, Vectors);
		UMaterialEditingLibrary::GetTextureParameterNames(Material, Textures);
		UMaterialEditingLibrary::GetStaticSwitchParameterNames(Material, Switches);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Material->GetPathName());
		AppendNames(TEXT("scalar"), Scalars, Result);
		AppendNames(TEXT("vector"), Vectors, Result);
		AppendNames(TEXT("texture"), Textures, Result);
		AppendNames(TEXT("staticSwitch"), Switches, Result);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"set_material_instance_scalar","description":"Set a scalar parameter on a material instance constant. Returns parameterExists=false if the parent has no such parameter.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parameter":{"type":"string"},"value":{"type":"number"}},"required":["assetPath","parameter","value"]},"annotations":{"title":"Set MI Scalar","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_material_instance_vector","description":"Set a vector/color parameter on a material instance constant. 'value' is {r,g,b,a} (a defaults to 1).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parameter":{"type":"string"},"value":{"type":"object"}},"required":["assetPath","parameter","value"]},"annotations":{"title":"Set MI Vector","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_material_instance_texture","description":"Set a texture parameter on a material instance constant.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parameter":{"type":"string"},"texture":{"type":"string"}},"required":["assetPath","parameter","texture"]},"annotations":{"title":"Set MI Texture","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_material_instance_switch","description":"Set a static switch parameter on a material instance constant.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parameter":{"type":"string"},"value":{"type":"boolean"}},"required":["assetPath","parameter","value"]},"annotations":{"title":"Set MI Static Switch","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"list_material_parameters","description":"List a material or material-instance's parameter names by type (scalar, vector, texture, staticSwitch).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"List Material Parameters","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("set_material_instance_scalar")) { OutResult = SetScalar(Args); return true; }
	if (ToolName == TEXT("set_material_instance_vector")) { OutResult = SetVector(Args); return true; }
	if (ToolName == TEXT("set_material_instance_texture")) { OutResult = SetTexture(Args); return true; }
	if (ToolName == TEXT("set_material_instance_switch")) { OutResult = SetSwitch(Args); return true; }
	if (ToolName == TEXT("list_material_parameters")) { OutResult = ListParameters(Args); return true; }
	return false;
}
}
}
