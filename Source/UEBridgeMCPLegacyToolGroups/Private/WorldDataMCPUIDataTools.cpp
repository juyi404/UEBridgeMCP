#include "WorldDataMCPUIDataTools.h"

#include "WorldDataMCPCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace WorldDataMCP
{
namespace UIDataTools
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

	// JSON value -> a string ImportText can consume.
	FString JsonValueToImportString(const TSharedPtr<FJsonValue>& V)
	{
		switch (V->Type)
		{
		case EJson::String:  return V->AsString();
		case EJson::Number:  return FString::SanitizeFloat(V->AsNumber());
		case EJson::Boolean: return V->AsBool() ? TEXT("True") : TEXT("False");
		default:             return V->AsString();
		}
	}

	// ---- data table ----------------------------------------------------------------------
	// NOTE: create_data_table and read_data_table are intentionally NOT defined here — the
	// older WorldDataMCPAuthoringTools already provides them. This module adds the row mutators
	// + JSON import below.

	FString ImportDataTableJson(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Json;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("json"), Json);
		UDataTable* DataTable = Cast<UDataTable>(LoadAssetObject(AssetPath));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("DataTable '%s' not found."), *AssetPath));
		}
		if (!DataTable->GetRowStruct())
		{
			return ErrorJson(TEXT("DataTable has no RowStruct."));
		}
		const TArray<FString> Errors = DataTable->CreateTableFromJSONString(Json);
		if (Errors.Num() > 0)
		{
			return ErrorJson(FString::Printf(TEXT("DataTable JSON import failed: %s"), *FString::Join(Errors, TEXT("; "))));
		}
		DataTable->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(DataTable, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), DataTable->GetPathName());
		Result->SetNumberField(TEXT("rowCount"), DataTable->GetRowMap().Num());
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& E : Errors) { ErrArr.Add(MakeShared<FJsonValueString>(E)); }
		Result->SetArrayField(TEXT("errors"), ErrArr);
		Result->SetBoolField(TEXT("ok"), true);
		return SuccessJson(Result);
	}

	FString SetDataTableRow(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, RowName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("rowName"), RowName);
		UDataTable* DataTable = Cast<UDataTable>(LoadAssetObject(AssetPath));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("DataTable '%s' not found."), *AssetPath));
		}
		UScriptStruct* RowStruct = const_cast<UScriptStruct*>(DataTable->GetRowStruct());
		if (!RowStruct)
		{
			return ErrorJson(TEXT("DataTable has no RowStruct."));
		}
		const TSharedPtr<FJsonObject>* RowObj = nullptr;
		if (!Args->TryGetObjectField(TEXT("row"), RowObj) || !RowObj || !(*RowObj).IsValid())
		{
			return ErrorJson(TEXT("Missing 'row' object (flat field name/value pairs)."));
		}
		const FName RowKey(*RowName);

		const int32 Size = RowStruct->GetStructureSize();
		const int32 Align = RowStruct->GetMinAlignment();
		uint8* NewRow = static_cast<uint8*>(FMemory::Malloc(Size, Align));
		RowStruct->InitializeStruct(NewRow);
		// Seed from an existing row so partial updates keep other fields.
		if (uint8* const* PrevPtr = DataTable->GetRowMap().Find(RowKey))
		{
			RowStruct->CopyScriptStruct(NewRow, *PrevPtr);
		}

		TArray<FString> Applied, Failed;
		for (const auto& KV : (*RowObj)->Values)
		{
			const FString FieldName(KV.Key);
			FProperty* FieldProp = nullptr;
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				if (It->GetName() == FieldName || It->GetAuthoredName() == FieldName)
				{
					FieldProp = *It;
					break;
				}
			}
			if (!FieldProp)
			{
				Failed.Add(FString::Printf(TEXT("%s: field not found"), *FieldName));
				continue;
			}
			void* FieldAddr = FieldProp->ContainerPtrToValuePtr<void>(NewRow);
			const FString ValueStr = JsonValueToImportString(KV.Value);
			if (FieldProp->ImportText_Direct(*ValueStr, FieldAddr, DataTable, PPF_None) == nullptr)
			{
				Failed.Add(FString::Printf(TEXT("%s: failed to set '%s'"), *FieldName, *ValueStr));
			}
			else
			{
				Applied.Add(FieldName);
			}
		}

		DataTable->RemoveRow(RowKey);
		DataTable->AddRow(RowKey, NewRow, RowStruct);
		RowStruct->DestroyStruct(NewRow);
		FMemory::Free(NewRow);

		DataTable->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(DataTable, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), DataTable->GetPathName());
		Result->SetStringField(TEXT("rowName"), RowName);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) { AppliedArr.Add(MakeShared<FJsonValueString>(A)); }
		Result->SetArrayField(TEXT("appliedFields"), AppliedArr);
		if (Failed.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FailedArr;
			for (const FString& F : Failed) { FailedArr.Add(MakeShared<FJsonValueString>(F)); }
			Result->SetArrayField(TEXT("failedFields"), FailedArr);
		}
		return SuccessJson(Result);
	}

	FString RemoveDataTableRow(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, RowName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("rowName"), RowName);
		UDataTable* DataTable = Cast<UDataTable>(LoadAssetObject(AssetPath));
		if (!DataTable)
		{
			return ErrorJson(FString::Printf(TEXT("DataTable '%s' not found."), *AssetPath));
		}
		const FName RowKey(*RowName);
		if (!DataTable->GetRowMap().Contains(RowKey))
		{
			return ErrorJson(FString::Printf(TEXT("Row '%s' does not exist."), *RowName));
		}
		DataTable->RemoveRow(RowKey);
		DataTable->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(DataTable, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), DataTable->GetPathName());
		Result->SetStringField(TEXT("removedRow"), RowName);
		Result->SetNumberField(TEXT("rowCount"), DataTable->GetRowMap().Num());
		return SuccessJson(Result);
	}

	// ---- widgets -------------------------------------------------------------------------
	// NOTE: create_widget_blueprint and add_widget are intentionally NOT defined here — the
	// older WorldDataMCPAuthoringTools already provides them. This module adds the read-only
	// tree reader + the property setter below.

	UWidget* FindWidgetByName(UWidgetTree* Tree, const FString& Name)
	{
		UWidget* Found = nullptr;
		Tree->ForEachWidget([&](UWidget* W)
		{
			if (!Found && W && W->GetName() == Name)
			{
				Found = W;
			}
		});
		return Found;
	}

	FString ReadWidgetTree(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadAssetObject(AssetPath));
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return ErrorJson(FString::Printf(TEXT("Widget blueprint '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Widgets;
		WidgetBP->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (!W) { return; }
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), W->GetName());
			E->SetStringField(TEXT("class"), W->GetClass()->GetName());
			E->SetStringField(TEXT("parent"), W->GetParent() ? W->GetParent()->GetName() : TEXT(""));
			E->SetBoolField(TEXT("isPanel"), W->IsA(UPanelWidget::StaticClass()));
			Widgets.Add(MakeShared<FJsonValueObject>(E));
		});
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), WidgetBP->GetPathName());
		Result->SetStringField(TEXT("root"), WidgetBP->WidgetTree->RootWidget ? WidgetBP->WidgetTree->RootWidget->GetName() : TEXT(""));
		Result->SetNumberField(TEXT("count"), Widgets.Num());
		Result->SetArrayField(TEXT("widgets"), Widgets);
		return SuccessJson(Result);
	}

	FString SetWidgetProperty(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, WidgetName, PropertyName, Value;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("widgetName"), WidgetName);
		Args->TryGetStringField(TEXT("property"), PropertyName);
		Args->TryGetStringField(TEXT("value"), Value);
		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(LoadAssetObject(AssetPath));
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return ErrorJson(FString::Printf(TEXT("Widget blueprint '%s' not found."), *AssetPath));
		}
		UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
		if (!Widget)
		{
			return ErrorJson(FString::Printf(TEXT("Widget '%s' not found."), *WidgetName));
		}

		WidgetBP->Modify();
		Widget->Modify();

		// Common, reliable special case: text on a text block.
		if (PropertyName.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
		{
			if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
			{
				TextBlock->SetText(FText::FromString(Value));
				FKismetEditorUtilities::CompileBlueprint(WidgetBP);
				WidgetBP->MarkPackageDirty();
				UEditorAssetLibrary::SaveLoadedAsset(WidgetBP, /*bOnlyIfIsDirty*/false);
				TSharedRef<FJsonObject> Ok = MakeShared<FJsonObject>();
				Ok->SetStringField(TEXT("widget"), Widget->GetName());
				Ok->SetStringField(TEXT("property"), PropertyName);
				Ok->SetStringField(TEXT("value"), Value);
				return SuccessJson(Ok);
			}
		}

		FProperty* Property = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			return ErrorJson(FString::Printf(TEXT("Property '%s' not found on %s."), *PropertyName, *Widget->GetClass()->GetName()));
		}
		void* Addr = Property->ContainerPtrToValuePtr<void>(Widget);
		if (Property->ImportText_Direct(*Value, Addr, Widget, PPF_None) == nullptr)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to set '%s' = '%s'."), *PropertyName, *Value));
		}
		FKismetEditorUtilities::CompileBlueprint(WidgetBP);
		WidgetBP->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(WidgetBP, /*bOnlyIfIsDirty*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget"), Widget->GetName());
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("value"), Value);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"import_data_table_json","description":"Replace a DataTable's rows from a JSON string (UDataTable::CreateTableFromJSONString). Returns any parse errors.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"json":{"type":"string"}},"required":["assetPath","json"]},"annotations":{"title":"Import Data Table JSON","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"set_data_table_row","description":"Add or update one row. 'row' is a flat {fieldName:value} object; existing fields are preserved when updating. Field values set via property reflection (numbers/strings/bools/enums).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"rowName":{"type":"string"},"row":{"type":"object"}},"required":["assetPath","rowName","row"]},"annotations":{"title":"Set Data Table Row","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"remove_data_table_row","description":"Remove a row by name from a DataTable.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"rowName":{"type":"string"}},"required":["assetPath","rowName"]},"annotations":{"title":"Remove Data Table Row","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"read_widget_tree","description":"List the widgets in a Widget Blueprint (name, class, parent, isPanel) plus the root name.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Widget Tree","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_widget_property","description":"Set a property on a widget in a Widget Blueprint (string value, applied via reflection; 'Text' on a TextBlock is special-cased to SetText).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"widgetName":{"type":"string"},"property":{"type":"string"},"value":{"type":"string"}},"required":["assetPath","widgetName","property","value"]},"annotations":{"title":"Set Widget Property","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("import_data_table_json")) { OutResult = ImportDataTableJson(Args); return true; }
	if (ToolName == TEXT("set_data_table_row")) { OutResult = SetDataTableRow(Args); return true; }
	if (ToolName == TEXT("remove_data_table_row")) { OutResult = RemoveDataTableRow(Args); return true; }
	if (ToolName == TEXT("read_widget_tree")) { OutResult = ReadWidgetTree(Args); return true; }
	if (ToolName == TEXT("set_widget_property")) { OutResult = SetWidgetProperty(Args); return true; }
	return false;
}
}
}
