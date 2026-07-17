#include "WorldDataMCPGasTools.h"

#include "WorldDataMCPCommon.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AttributeSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameplayEffect.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace GasTools
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

	// Create a data-only blueprint with a given native parent class, save it, return it.
	UBlueprint* CreateGasBlueprint(UClass* ParentClass, const FString& Name, const FString& PackagePath, FString& OutError)
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
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, *Name, BPTYPE_Normal);
		if (!BP)
		{
			OutError = TEXT("CreateBlueprint failed.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(BP);
		Package->MarkPackageDirty();
		UEditorAssetLibrary::SaveAsset(PackageFullPath, /*bOnlyIfIsDirty*/false);
		return BP;
	}

	UBlueprint* LoadBlueprint(const FString& Path)
	{
		return Cast<UBlueprint>(LoadAssetObject(Path));
	}

	// Find a FGameplayAttribute by "Name" or "SetFilter.Name" across loaded UAttributeSet subclasses.
	FGameplayAttribute FindGameplayAttributeByName(const FString& InName)
	{
		FString SetFilter, AttrName = InName;
		if (InName.Contains(TEXT(".")))
		{
			InName.Split(TEXT("."), &SetFilter, &AttrName);
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* C = *It;
			if (C == UAttributeSet::StaticClass() || !C->IsChildOf(UAttributeSet::StaticClass()))
			{
				continue;
			}
			if (!SetFilter.IsEmpty() && !C->GetName().Contains(SetFilter))
			{
				continue;
			}
			for (TFieldIterator<FProperty> P(C); P; ++P)
			{
				FStructProperty* SP = CastField<FStructProperty>(*P);
				if (SP && SP->Struct == FGameplayAttributeData::StaticStruct() && SP->GetName() == AttrName)
				{
					return FGameplayAttribute(SP);
				}
			}
		}
		return FGameplayAttribute();
	}

	FString MakeCreateResult(UBlueprint* BP, const TCHAR* Kind)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), BP->GetPathName());
		Result->SetStringField(TEXT("name"), BP->GetName());
		Result->SetStringField(TEXT("kind"), Kind);
		return SuccessJson(Result);
	}

	// ---- tools ---------------------------------------------------------------------------

	// NOTE: create_gameplay_effect and create_gameplay_ability are intentionally NOT defined
	// here — the older WorldDataMCPAuthoringTools already provides them (destPath schema). This
	// module adds attribute-set creation, attribute/component authoring, and effect modifiers.

	FString CreateAttributeSet(const TSharedPtr<FJsonObject>& Args)
	{
		FString Name; Args->TryGetStringField(TEXT("name"), Name);
		if (Name.IsEmpty()) { return ErrorJson(TEXT("Missing 'name'.")); }
		FString PackagePath = TEXT("/Game/GAS/Attributes");
		Args->TryGetStringField(TEXT("packagePath"), PackagePath);
		FString Error;
		UBlueprint* BP = CreateGasBlueprint(UAttributeSet::StaticClass(), Name, PackagePath, Error);
		if (!BP) { return ErrorJson(Error); }
		return MakeCreateResult(BP, TEXT("AttributeSet"));
	}

	FString AddAttribute(const TSharedPtr<FJsonObject>& Args)
	{
		FString BPPath, AttrName;
		Args->TryGetStringField(TEXT("attributeSetPath"), BPPath);
		Args->TryGetStringField(TEXT("attributeName"), AttrName);
		UBlueprint* BP = LoadBlueprint(BPPath);
		if (!BP) { return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *BPPath)); }
		if (AttrName.IsEmpty()) { return ErrorJson(TEXT("Missing 'attributeName'.")); }

		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = FGameplayAttributeData::StaticStruct();
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(*AttrName), PinType))
		{
			return ErrorJson(FString::Printf(TEXT("Failed to add attribute '%s' (does it already exist?)."), *AttrName));
		}
		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), BP->GetPathName());
		Result->SetStringField(TEXT("attribute"), AttrName);
		return SuccessJson(Result);
	}

	FString AddAbilitySystemComponent(const TSharedPtr<FJsonObject>& Args)
	{
		FString BPPath, CompName;
		Args->TryGetStringField(TEXT("blueprintPath"), BPPath);
		Args->TryGetStringField(TEXT("componentName"), CompName);
		if (CompName.IsEmpty()) { CompName = TEXT("AbilitySystemComponent"); }
		UBlueprint* BP = LoadBlueprint(BPPath);
		if (!BP) { return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *BPPath)); }
		if (!BP->SimpleConstructionScript)
		{
			return ErrorJson(TEXT("Blueprint has no SimpleConstructionScript (not an actor blueprint?)."));
		}
		UClass* ASCClass = UAbilitySystemComponent::StaticClass();
		for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (N && N->ComponentTemplate && N->ComponentTemplate->GetClass() == ASCClass)
			{
				return ErrorJson(TEXT("Blueprint already has an AbilitySystemComponent."));
			}
		}
		USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(ASCClass, *CompName);
		if (!NewNode)
		{
			return ErrorJson(TEXT("CreateNode failed."));
		}
		BP->SimpleConstructionScript->AddNode(NewNode);
		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), BP->GetPathName());
		Result->SetStringField(TEXT("component"), CompName);
		return SuccessJson(Result);
	}

	FString SetEffectModifier(const TSharedPtr<FJsonObject>& Args)
	{
		FString EffectPath, Attribute, Op = TEXT("additive");
		Args->TryGetStringField(TEXT("effectPath"), EffectPath);
		Args->TryGetStringField(TEXT("attribute"), Attribute);
		Args->TryGetStringField(TEXT("op"), Op);
		double Magnitude = 0.0;
		Args->TryGetNumberField(TEXT("magnitude"), Magnitude);

		UBlueprint* BP = LoadBlueprint(EffectPath);
		if (!BP || !BP->GeneratedClass)
		{
			return ErrorJson(FString::Printf(TEXT("GameplayEffect blueprint '%s' not found."), *EffectPath));
		}
		UGameplayEffect* GE = Cast<UGameplayEffect>(BP->GeneratedClass->GetDefaultObject());
		if (!GE)
		{
			return ErrorJson(TEXT("Blueprint is not a GameplayEffect."));
		}
		const FGameplayAttribute GAttr = FindGameplayAttributeByName(Attribute);
		if (!GAttr.IsValid())
		{
			return ErrorJson(FString::Printf(TEXT("Gameplay attribute '%s' not found (is its AttributeSet loaded?)."), *Attribute));
		}
		EGameplayModOp::Type ModOp = EGameplayModOp::Additive;
		if (Op.Equals(TEXT("multiplicative"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("multiply"), ESearchCase::IgnoreCase)) { ModOp = EGameplayModOp::Multiplicitive; }
		else if (Op.Equals(TEXT("division"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("divide"), ESearchCase::IgnoreCase)) { ModOp = EGameplayModOp::Division; }
		else if (Op.Equals(TEXT("override"), ESearchCase::IgnoreCase)) { ModOp = EGameplayModOp::Override; }

		const FGameplayEffectModifierMagnitude Mag(FScalableFloat(static_cast<float>(Magnitude)));
		bool bUpdated = false;
		for (FGameplayModifierInfo& M : GE->Modifiers)
		{
			if (M.Attribute == GAttr && M.ModifierOp == ModOp)
			{
				M.ModifierMagnitude = Mag;
				bUpdated = true;
				break;
			}
		}
		if (!bUpdated)
		{
			FGameplayModifierInfo Info;
			Info.Attribute = GAttr;
			Info.ModifierOp = ModOp;
			Info.ModifierMagnitude = Mag;
			GE->Modifiers.Add(Info);
		}
		// Data-only GE blueprint: the CDO holds the defaults, so persist without recompiling.
		GE->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), BP->GetPathName());
		Result->SetStringField(TEXT("attribute"), Attribute);
		Result->SetStringField(TEXT("op"), Op);
		Result->SetNumberField(TEXT("magnitude"), Magnitude);
		Result->SetBoolField(TEXT("updatedExisting"), bUpdated);
		Result->SetNumberField(TEXT("modifierCount"), GE->Modifiers.Num());
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_attribute_set","description":"Create an AttributeSet blueprint (parent UAttributeSet). Add attributes with add_attribute.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"packagePath":{"type":"string","description":"Default /Game/GAS/Attributes."}},"required":["name"]},"annotations":{"title":"Create Attribute Set","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_attribute","description":"Add a FGameplayAttributeData member variable to an attribute set blueprint.","inputSchema":{"type":"object","properties":{"attributeSetPath":{"type":"string"},"attributeName":{"type":"string"}},"required":["attributeSetPath","attributeName"]},"annotations":{"title":"Add Attribute","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_ability_system_component","description":"Add an AbilitySystemComponent as an SCS node to an actor blueprint.","inputSchema":{"type":"object","properties":{"blueprintPath":{"type":"string"},"componentName":{"type":"string"}},"required":["blueprintPath"]},"annotations":{"title":"Add Ability System Component","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"set_effect_modifier","description":"Add or update a modifier on a GameplayEffect's defaults. attribute: 'Health' or 'MyAttributeSet.Health'. op: additive (default), multiplicative, division, override. magnitude: a scalar. The attribute's AttributeSet must be loaded.","inputSchema":{"type":"object","properties":{"effectPath":{"type":"string"},"attribute":{"type":"string"},"op":{"type":"string"},"magnitude":{"type":"number"}},"required":["effectPath","attribute","magnitude"]},"annotations":{"title":"Set Effect Modifier","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_attribute_set")) { OutResult = CreateAttributeSet(Args); return true; }
	if (ToolName == TEXT("add_attribute")) { OutResult = AddAttribute(Args); return true; }
	if (ToolName == TEXT("add_ability_system_component")) { OutResult = AddAbilitySystemComponent(Args); return true; }
	if (ToolName == TEXT("set_effect_modifier")) { OutResult = SetEffectModifier(Args); return true; }
	return false;
}
}
}
