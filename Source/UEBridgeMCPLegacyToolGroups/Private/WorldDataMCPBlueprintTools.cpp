#include "WorldDataMCPBlueprintTools.h"

#include "WorldDataMCPCommon.h"

#include "BlueprintEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "SubobjectDataHandle.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace BlueprintTools
{
namespace
{
	UClass* ResolveClass(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")))
		{
			if (UClass* C = LoadObject<UClass>(nullptr, *Name)) { return C; }
		}
		if (UClass* C = FindObject<UClass>(nullptr, *Name)) { return C; }
		if (UClass* C = LoadObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + Name))) { return C; }
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Name) { return *It; }
		}
		return nullptr;
	}

	UBlueprint* LoadBlueprint(const FString& Path)
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
		return LoadObject<UBlueprint>(nullptr, *Normalized);
	}

	void Save(UBlueprint* BP, bool bStructural)
	{
		if (!BP) { return; }
		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		}
		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(BP, /*bOnlyIfIsDirty*/false);
	}

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
		else if (T == TEXT("vector")) { P.PinCategory = UEdGraphSchema_K2::PC_Struct; P.PinSubCategoryObject = TBaseStructure<FVector>::Get(); }
		else if (T == TEXT("rotator")) { P.PinCategory = UEdGraphSchema_K2::PC_Struct; P.PinSubCategoryObject = TBaseStructure<FRotator>::Get(); }
		else if (T == TEXT("transform")) { P.PinCategory = UEdGraphSchema_K2::PC_Struct; P.PinSubCategoryObject = TBaseStructure<FTransform>::Get(); }
		else if (T == TEXT("linearcolor") || T == TEXT("color")) { P.PinCategory = UEdGraphSchema_K2::PC_Struct; P.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get(); }
		else if (UClass* AsClass = ResolveClass(Type))
		{
			// Treat a resolvable class name as an object reference pin.
			P.PinCategory = UEdGraphSchema_K2::PC_Object;
			P.PinSubCategoryObject = AsClass;
		}
		else
		{
			P.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		return P;
	}

	UEdGraph* FindFunctionGraph(UBlueprint* BP, const FString& FunctionName)
	{
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetName() == FunctionName) { return G; }
		}
		return nullptr;
	}

	// ---- tools ---------------------------------------------------------------------------

	FString AddComponent(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, ComponentClass, ComponentName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("componentClass"), ComponentClass);
		Args->TryGetStringField(TEXT("name"), ComponentName);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		UClass* CompClass = ResolveClass(ComponentClass);
		if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return ErrorJson(FString::Printf(TEXT("'%s' is not a component class."), *ComponentClass));
		}
		USubobjectDataSubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<USubobjectDataSubsystem>() : nullptr;
		if (!Subsystem)
		{
			return ErrorJson(TEXT("SubobjectDataSubsystem unavailable."));
		}
		TArray<FSubobjectDataHandle> Handles;
		Subsystem->K2_GatherSubobjectDataForBlueprint(BP, Handles);
		FSubobjectDataHandle RootHandle = Handles.Num() > 0 ? Handles[0] : FSubobjectDataHandle();

		FAddNewSubobjectParams AddParams;
		AddParams.ParentHandle = RootHandle;
		AddParams.NewClass = CompClass;
		AddParams.BlueprintContext = BP;
		FText FailReason;
		FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(AddParams, FailReason);
		if (!NewHandle.IsValid())
		{
			return ErrorJson(FString::Printf(TEXT("AddNewSubobject failed: %s"), *FailReason.ToString()));
		}
		if (!ComponentName.IsEmpty() && ComponentName != ComponentClass)
		{
			Subsystem->RenameSubobject(NewHandle, FText::FromString(ComponentName));
		}
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("componentClass"), CompClass->GetName());
		Result->SetStringField(TEXT("name"), ComponentName.IsEmpty() ? CompClass->GetName() : ComponentName);
		return SuccessJson(Result);
	}

	FString AddInterface(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, InterfacePath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("interface"), InterfacePath);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		UClass* InterfaceClass = ResolveClass(InterfacePath);
		if (!InterfaceClass)
		{
			return ErrorJson(FString::Printf(TEXT("Interface '%s' not found."), *InterfacePath));
		}
		for (const FBPInterfaceDescription& Impl : BP->ImplementedInterfaces)
		{
			if (Impl.Interface == InterfaceClass)
			{
				return ErrorJson(TEXT("Interface already implemented."));
			}
		}
		FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(InterfaceClass->GetPathName()));
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("interface"), InterfaceClass->GetPathName());
		return SuccessJson(Result);
	}

	FString AddEventDispatcher(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, Name;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("name"), Name);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP || Name.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' and 'name' are required."));
		}
		const FName DispatcherName(*Name);

		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, DispatcherName, DelegateType))
		{
			return ErrorJson(FString::Printf(TEXT("Failed to add dispatcher variable '%s' (name may exist)."), *Name));
		}
		UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(BP, DispatcherName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!SigGraph)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(BP, DispatcherName);
			return ErrorJson(TEXT("Failed to create dispatcher signature graph."));
		}
		SigGraph->bEditable = false;
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		K2Schema->CreateDefaultNodesForGraph(*SigGraph);
		K2Schema->CreateFunctionGraphTerminators(*SigGraph, (UClass*)nullptr);
		K2Schema->AddExtraFunctionFlags(SigGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
		K2Schema->MarkFunctionEntryAsEditable(SigGraph, true);
		BP->DelegateSignatureGraphs.Add(SigGraph);

		// Optional dispatcher parameters.
		const TArray<TSharedPtr<FJsonValue>>* ParamsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("parameters"), ParamsArr) && ParamsArr)
		{
			TArray<UK2Node_EditablePinBase*> EntryNodes;
			SigGraph->GetNodesOfClass<UK2Node_EditablePinBase>(EntryNodes);
			if (EntryNodes.Num() > 0 && EntryNodes[0])
			{
				for (const TSharedPtr<FJsonValue>& V : *ParamsArr)
				{
					const TSharedPtr<FJsonObject> Obj = V->AsObject();
					if (!Obj.IsValid()) { continue; }
					FString PName, PType = TEXT("float");
					Obj->TryGetStringField(TEXT("name"), PName);
					Obj->TryGetStringField(TEXT("type"), PType);
					if (!PName.IsEmpty())
					{
						EntryNodes[0]->CreateUserDefinedPin(FName(*PName), MakePinType(PType), EGPD_Output);
					}
				}
			}
		}
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("dispatcher"), Name);
		return SuccessJson(Result);
	}

	FString AddFunction(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, FunctionName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("name"), FunctionName);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP || FunctionName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' and 'name' are required."));
		}
		if (FindFunctionGraph(BP, FunctionName))
		{
			return ErrorJson(FString::Printf(TEXT("Function '%s' already exists."), *FunctionName));
		}
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			return ErrorJson(TEXT("Failed to create function graph."));
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/true, (UClass*)nullptr);
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("function"), FunctionName);
		return SuccessJson(Result);
	}

	FString AddCustomEvent(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, EventName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("name"), EventName);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP || EventName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' and 'name' are required."));
		}
		UEdGraph* EventGraph = (BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
		if (!EventGraph)
		{
			return ErrorJson(TEXT("Blueprint has no event graph."));
		}
		UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(EventGraph);
		Node->CustomFunctionName = FName(*EventName);
		EventGraph->Modify();
		EventGraph->AddNode(Node, /*bFromUI*/false, /*bSelectNewNode*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		double PosX = 0.0, PosY = 0.0;
		if (Args->TryGetNumberField(TEXT("posX"), PosX)) { Node->NodePosX = static_cast<int32>(PosX); }
		if (Args->TryGetNumberField(TEXT("posY"), PosY)) { Node->NodePosY = static_cast<int32>(PosY); }
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("event"), EventName);
		Result->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
		return SuccessJson(Result);
	}

	FString AddFunctionParameter(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, FunctionName, ParamName, ParamType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("function"), FunctionName);
		Args->TryGetStringField(TEXT("paramName"), ParamName);
		Args->TryGetStringField(TEXT("paramType"), ParamType);
		bool bOutput = false;
		Args->TryGetBoolField(TEXT("output"), bOutput);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP || ParamName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' and 'paramName' are required."));
		}
		UEdGraph* FuncGraph = FindFunctionGraph(BP, FunctionName);
		if (!FuncGraph)
		{
			return ErrorJson(FString::Printf(TEXT("Function '%s' not found."), *FunctionName));
		}
		const FEdGraphPinType PinType = MakePinType(ParamType);

		if (!bOutput)
		{
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* N : FuncGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Entry = E; break; }
			}
			if (!Entry)
			{
				return ErrorJson(TEXT("Function entry node not found."));
			}
			Entry->Modify();
			Entry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
		}
		else
		{
			UK2Node_EditablePinBase* ResultNode = nullptr;
			for (UEdGraphNode* N : FuncGraph->Nodes)
			{
				if (N && N->GetClass()->GetName() == TEXT("K2Node_FunctionResult"))
				{
					ResultNode = Cast<UK2Node_EditablePinBase>(N);
					break;
				}
			}
			if (!ResultNode)
			{
				return ErrorJson(TEXT("Function has no result node; add a return value in-editor first."));
			}
			ResultNode->Modify();
			ResultNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input);
		}
		Save(BP, /*bStructural*/true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("function"), FunctionName);
		Result->SetStringField(TEXT("paramName"), ParamName);
		Result->SetBoolField(TEXT("output"), bOutput);
		return SuccessJson(Result);
	}

	FString AddLocalVariable(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, FunctionName, VarName, VarType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("function"), FunctionName);
		Args->TryGetStringField(TEXT("name"), VarName);
		Args->TryGetStringField(TEXT("type"), VarType);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP || VarName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' and 'name' are required."));
		}
		UEdGraph* FuncGraph = FindFunctionGraph(BP, FunctionName);
		if (!FuncGraph)
		{
			return ErrorJson(FString::Printf(TEXT("Function '%s' not found."), *FunctionName));
		}
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* N : FuncGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N)) { Entry = E; break; }
		}
		if (!Entry)
		{
			return ErrorJson(TEXT("Function entry node not found."));
		}
		const FName VarFName(*VarName);
		for (const FBPVariableDescription& V : Entry->LocalVariables)
		{
			if (V.VarName == VarFName)
			{
				return ErrorJson(TEXT("Local variable already exists."));
			}
		}
		FBPVariableDescription NewVar;
		NewVar.VarName = VarFName;
		NewVar.VarGuid = FGuid::NewGuid();
		NewVar.VarType = MakePinType(VarType);
		NewVar.FriendlyName = VarName;
		Entry->Modify();
		Entry->LocalVariables.Add(NewVar);
		Save(BP, /*bStructural*/false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("function"), FunctionName);
		Result->SetStringField(TEXT("variable"), VarName);
		return SuccessJson(Result);
	}

	FString ReparentBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, ParentName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("parentClass"), ParentName);
		UBlueprint* BP = LoadBlueprint(AssetPath);
		if (!BP)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		UClass* NewParent = ResolveClass(ParentName);
		if (!NewParent)
		{
			return ErrorJson(FString::Printf(TEXT("Parent class '%s' not found."), *ParentName));
		}
		if (NewParent->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return ErrorJson(TEXT("Parent class is deprecated."));
		}
		if (BP->GeneratedClass && (NewParent == BP->GeneratedClass || NewParent->IsChildOf(BP->GeneratedClass)))
		{
			return ErrorJson(TEXT("Parent would create a class cycle."));
		}
		UBlueprintEditorLibrary::ReparentBlueprint(BP, NewParent);
		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();
		UEditorAssetLibrary::SaveLoadedAsset(BP, false);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
		Result->SetStringField(TEXT("parentClass"), NewParent->GetName());
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"add_blueprint_component","description":"Add a component to a Blueprint's construction script (SCS), e.g. StaticMeshComponent, PointLightComponent. Adds to the BP class, not a level actor.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"componentClass":{"type":"string"},"name":{"type":"string","description":"Optional component variable name."}},"required":["assetPath","componentClass"]},"annotations":{"title":"Add Blueprint Component","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_interface","description":"Implement a Blueprint Interface on a Blueprint.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"interface":{"type":"string","description":"Interface class name or asset path."}},"required":["assetPath","interface"]},"annotations":{"title":"Add Blueprint Interface","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_event_dispatcher","description":"Add an event dispatcher (multicast delegate) to a Blueprint, with optional typed parameters ([{name,type}]).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"},"parameters":{"type":"array","items":{"type":"object"}}},"required":["assetPath","name"]},"annotations":{"title":"Add Event Dispatcher","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_function","description":"Add a new (empty) function graph to a Blueprint.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"}},"required":["assetPath","name"]},"annotations":{"title":"Add Blueprint Function","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_custom_event","description":"Add a Custom Event node to the Blueprint's event graph.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"},"posX":{"type":"number"},"posY":{"type":"number"}},"required":["assetPath","name"]},"annotations":{"title":"Add Custom Event","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_function_parameter","description":"Add an input (default) or output (output=true) parameter to a Blueprint function. Output needs an existing return node. Types: bool/byte/int/int64/float/string/name/text/vector/rotator/transform/color or a class name.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"function":{"type":"string"},"paramName":{"type":"string"},"paramType":{"type":"string"},"output":{"type":"boolean"}},"required":["assetPath","function","paramName"]},"annotations":{"title":"Add Function Parameter","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blueprint_local_variable","description":"Add a local variable to a Blueprint function.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"function":{"type":"string"},"name":{"type":"string"},"type":{"type":"string"}},"required":["assetPath","function","name"]},"annotations":{"title":"Add Local Variable","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"reparent_blueprint","description":"Change a Blueprint's parent class (validates against deprecated parents and class cycles), then compiles.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"parentClass":{"type":"string"}},"required":["assetPath","parentClass"]},"annotations":{"title":"Reparent Blueprint","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("add_blueprint_component")) { OutResult = AddComponent(Args); return true; }
	if (ToolName == TEXT("add_blueprint_interface")) { OutResult = AddInterface(Args); return true; }
	if (ToolName == TEXT("add_blueprint_event_dispatcher")) { OutResult = AddEventDispatcher(Args); return true; }
	if (ToolName == TEXT("add_blueprint_function")) { OutResult = AddFunction(Args); return true; }
	if (ToolName == TEXT("add_blueprint_custom_event")) { OutResult = AddCustomEvent(Args); return true; }
	if (ToolName == TEXT("add_blueprint_function_parameter")) { OutResult = AddFunctionParameter(Args); return true; }
	if (ToolName == TEXT("add_blueprint_local_variable")) { OutResult = AddLocalVariable(Args); return true; }
	if (ToolName == TEXT("reparent_blueprint")) { OutResult = ReparentBlueprint(Args); return true; }
	return false;
}
}
}
