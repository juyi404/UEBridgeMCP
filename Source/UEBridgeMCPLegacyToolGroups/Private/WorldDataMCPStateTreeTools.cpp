#include "WorldDataMCPStateTreeTools.h"

#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StateTreeTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace StateTreeTools
{
namespace
{
	UStateTree* LoadStateTree(const FString& Path)
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
		return LoadObject<UStateTree>(nullptr, *Normalized);
	}

	UStateTreeEditorData* GetEditorData(UStateTree* ST)
	{
		return ST ? Cast<UStateTreeEditorData>(ST->EditorData) : nullptr;
	}

	// Find a state by name across all subtrees (depth-first). Empty name returns the first subtree.
	UStateTreeState* ResolveState(UStateTreeEditorData* EditorData, const FString& Name)
	{
		if (!EditorData)
		{
			return nullptr;
		}
		if (Name.IsEmpty())
		{
			return EditorData->SubTrees.Num() > 0 ? EditorData->SubTrees[0] : nullptr;
		}
		TArray<UStateTreeState*> Stack;
		for (UStateTreeState* Sub : EditorData->SubTrees)
		{
			Stack.Add(Sub);
		}
		while (Stack.Num() > 0)
		{
			UStateTreeState* Cur = Stack.Pop();
			if (!Cur)
			{
				continue;
			}
			if (Cur->Name == FName(*Name))
			{
				return Cur;
			}
			for (UStateTreeState* Child : Cur->Children)
			{
				Stack.Add(Child);
			}
		}
		return nullptr;
	}

	UScriptStruct* ResolveStructType(const FString& Name)
	{
		if (UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *Name))
		{
			return S;
		}
		return FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst);
	}

	void SetInstanceProps(FInstancedStruct& Instance, const TSharedPtr<FJsonObject>& Props)
	{
		const UScriptStruct* Struct = Instance.GetScriptStruct();
		uint8* Memory = Instance.GetMutableMemory();
		if (!Struct || !Memory || !Props.IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			FProperty* Prop = Struct->FindPropertyByName(FName(*Pair.Key));
			if (!Prop)
			{
				continue;
			}
			FString ValueStr;
			switch (Pair.Value->Type)
			{
			case EJson::Number: ValueStr = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
			case EJson::Boolean: ValueStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
			default: ValueStr = Pair.Value->AsString(); break;
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Memory);
			Prop->ImportText_Direct(*ValueStr, ValuePtr, nullptr, PPF_None);
		}
	}

	// Construct an FStateTreeEditorNode of the given struct type into the array, optionally with
	// instance properties. RequiredBase enforces task/evaluator/condition base type.
	bool AddEditorNode(TArray<FStateTreeEditorNode>& Array, const FString& StructType, const UScriptStruct* RequiredBase,
		const TSharedPtr<FJsonObject>& InstanceProps, FStateTreeEditorNode*& OutNode, FString& OutError)
	{
		UScriptStruct* NodeStruct = ResolveStructType(StructType);
		if (!NodeStruct)
		{
			OutError = FString::Printf(TEXT("Node struct type '%s' not found."), *StructType);
			return false;
		}
		if (RequiredBase && !NodeStruct->IsChildOf(RequiredBase))
		{
			OutError = FString::Printf(TEXT("'%s' is not a %s."), *StructType, *RequiredBase->GetName());
			return false;
		}
		// IsChildOf() above is also true for RequiredBase itself, but a bare task/condition/
		// evaluator base is abstract and not an instantiable node. Reject it explicitly so we
		// never build an FInstancedStruct without a concrete FStateTreeNodeBase underneath.
		if (RequiredBase && NodeStruct == RequiredBase)
		{
			OutError = FString::Printf(TEXT("'%s' is the abstract base type; specify a concrete %s subclass."), *StructType, *RequiredBase->GetName());
			return false;
		}
		FStateTreeEditorNode& Node = Array.AddDefaulted_GetRef();
		Node.ID = FGuid::NewGuid();
		Node.Node.InitializeAs(NodeStruct);
		// Use GetPtr<>() (returns null on type mismatch) rather than Get<>() (which check()-
		// crashes the whole editor) so a struct that fails to resolve as a node base can be
		// reported as an error instead of taking the editor down.
		const FStateTreeNodeBase* NodeBase = Node.Node.GetPtr<FStateTreeNodeBase>();
		if (!NodeBase)
		{
			Array.Pop();
			OutError = FString::Printf(TEXT("'%s' could not be initialized as a state tree node."), *StructType);
			return false;
		}
		if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodeBase->GetInstanceDataType()))
		{
			Node.Instance.InitializeAs(InstanceType);
		}
		if (InstanceProps.IsValid() && Node.Instance.IsValid())
		{
			SetInstanceProps(Node.Instance, InstanceProps);
		}
		OutNode = &Node;
		return true;
	}

	void Save(UStateTree* ST)
	{
		if (ST)
		{
			ST->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(ST, /*bOnlyIfIsDirty*/false);
		}
	}

	EStateTreeTransitionTrigger ParseTrigger(const FString& Str)
	{
		if (Str.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionTrigger::OnTick; }
		if (Str.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionTrigger::OnEvent; }
		if (Str.Equals(TEXT("OnStateSucceeded"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionTrigger::OnStateSucceeded; }
		if (Str.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionTrigger::OnStateFailed; }
		return EStateTreeTransitionTrigger::OnStateCompleted;
	}

	EStateTreeTransitionType ParseTransitionType(const FString& Str)
	{
		if (Str.Equals(TEXT("Succeeded"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionType::Succeeded; }
		if (Str.Equals(TEXT("Failed"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionType::Failed; }
		if (Str.Equals(TEXT("NextState"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionType::NextState; }
		if (Str.Equals(TEXT("NextSelectableState"), ESearchCase::IgnoreCase)) { return EStateTreeTransitionType::NextSelectableState; }
		return EStateTreeTransitionType::GotoState;
	}

	// ---- tools ---------------------------------------------------------------------------

	struct FStateTreeContext
	{
		UStateTree* ST = nullptr;
		UStateTreeEditorData* EditorData = nullptr;
		FString Error;
	};

	bool OpenContext(const TSharedPtr<FJsonObject>& Args, FStateTreeContext& Ctx)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Ctx.ST = LoadStateTree(AssetPath);
		if (!Ctx.ST)
		{
			Ctx.Error = FString::Printf(TEXT("StateTree '%s' not found."), *AssetPath);
			return false;
		}
		Ctx.EditorData = GetEditorData(Ctx.ST);
		if (!Ctx.EditorData)
		{
			Ctx.Error = TEXT("StateTree has no editor data.");
			return false;
		}
		return true;
	}

	FString AddNodeToState(const TSharedPtr<FJsonObject>& Args, const UScriptStruct* RequiredBase, bool bEnterCondition)
	{
		FStateTreeContext Ctx;
		if (!OpenContext(Args, Ctx)) { return ErrorJson(Ctx.Error); }
		FString StateName;
		Args->TryGetStringField(TEXT("state"), StateName);
		UStateTreeState* State = ResolveState(Ctx.EditorData, StateName);
		if (!State)
		{
			return ErrorJson(FString::Printf(TEXT("State '%s' not found."), *StateName));
		}
		FString StructType;
		Args->TryGetStringField(TEXT("structType"), StructType);
		const TSharedPtr<FJsonObject>* InstPtr = nullptr;
		Args->TryGetObjectField(TEXT("instance"), InstPtr);
		const TSharedPtr<FJsonObject> Inst = InstPtr ? *InstPtr : nullptr;

		State->Modify();
		FStateTreeEditorNode* NewNode = nullptr;
		FString Err;
		TArray<FStateTreeEditorNode>& TargetArray = bEnterCondition ? State->EnterConditions : State->Tasks;
		if (!AddEditorNode(TargetArray, StructType, RequiredBase, Inst, NewNode, Err))
		{
			return ErrorJson(Err);
		}
		Save(Ctx.ST);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Ctx.ST->GetPathName());
		Result->SetStringField(TEXT("state"), State->Name.ToString());
		Result->SetStringField(TEXT("structType"), StructType);
		Result->SetStringField(TEXT("nodeId"), NewNode->ID.ToString());
		return SuccessJson(Result);
	}

	FString AddTask(const TSharedPtr<FJsonObject>& Args)
	{
		return AddNodeToState(Args, FStateTreeTaskBase::StaticStruct(), /*bEnterCondition*/false);
	}

	FString AddEnterCondition(const TSharedPtr<FJsonObject>& Args)
	{
		return AddNodeToState(Args, FStateTreeConditionBase::StaticStruct(), /*bEnterCondition*/true);
	}

	FString AddNodeToEditorData(const TSharedPtr<FJsonObject>& Args, const UScriptStruct* RequiredBase, bool bGlobalTask)
	{
		FStateTreeContext Ctx;
		if (!OpenContext(Args, Ctx)) { return ErrorJson(Ctx.Error); }
		FString StructType;
		Args->TryGetStringField(TEXT("structType"), StructType);
		const TSharedPtr<FJsonObject>* InstPtr = nullptr;
		Args->TryGetObjectField(TEXT("instance"), InstPtr);
		const TSharedPtr<FJsonObject> Inst = InstPtr ? *InstPtr : nullptr;

		Ctx.EditorData->Modify();
		FStateTreeEditorNode* NewNode = nullptr;
		FString Err;
		TArray<FStateTreeEditorNode>& TargetArray = bGlobalTask ? Ctx.EditorData->GlobalTasks : Ctx.EditorData->Evaluators;
		if (!AddEditorNode(TargetArray, StructType, RequiredBase, Inst, NewNode, Err))
		{
			return ErrorJson(Err);
		}
		Save(Ctx.ST);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Ctx.ST->GetPathName());
		Result->SetStringField(TEXT("structType"), StructType);
		Result->SetStringField(TEXT("nodeId"), NewNode->ID.ToString());
		Result->SetStringField(TEXT("target"), bGlobalTask ? TEXT("globalTasks") : TEXT("evaluators"));
		return SuccessJson(Result);
	}

	FString AddEvaluator(const TSharedPtr<FJsonObject>& Args)
	{
		return AddNodeToEditorData(Args, FStateTreeEvaluatorBase::StaticStruct(), /*bGlobalTask*/false);
	}

	FString AddGlobalTask(const TSharedPtr<FJsonObject>& Args)
	{
		return AddNodeToEditorData(Args, FStateTreeTaskBase::StaticStruct(), /*bGlobalTask*/true);
	}

	FString AddTransition(const TSharedPtr<FJsonObject>& Args)
	{
		FStateTreeContext Ctx;
		if (!OpenContext(Args, Ctx)) { return ErrorJson(Ctx.Error); }
		FString StateName;
		Args->TryGetStringField(TEXT("state"), StateName);
		UStateTreeState* State = ResolveState(Ctx.EditorData, StateName);
		if (!State)
		{
			return ErrorJson(FString::Printf(TEXT("State '%s' not found."), *StateName));
		}

		FString TriggerStr, TypeStr, TargetName;
		Args->TryGetStringField(TEXT("trigger"), TriggerStr);
		Args->TryGetStringField(TEXT("type"), TypeStr);
		Args->TryGetStringField(TEXT("targetState"), TargetName);
		const EStateTreeTransitionTrigger Trigger = ParseTrigger(TriggerStr);
		EStateTreeTransitionType Type = TypeStr.IsEmpty()
			? (TargetName.IsEmpty() ? EStateTreeTransitionType::Succeeded : EStateTreeTransitionType::GotoState)
			: ParseTransitionType(TypeStr);

		State->Modify();
		FStateTreeTransition& Trans = State->AddTransition(Trigger, Type, nullptr);
		if (Type == EStateTreeTransitionType::GotoState && !TargetName.IsEmpty())
		{
			if (UStateTreeState* Target = ResolveState(Ctx.EditorData, TargetName))
			{
				Trans.State = Target->GetLinkToState();
			}
		}
		double DelayDuration = 0.0;
		if (Args->TryGetNumberField(TEXT("delayDuration"), DelayDuration))
		{
			Trans.bDelayTransition = true;
			Trans.DelayDuration = static_cast<float>(DelayDuration);
		}
		Save(Ctx.ST);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Ctx.ST->GetPathName());
		Result->SetStringField(TEXT("state"), State->Name.ToString());
		Result->SetNumberField(TEXT("transitionIndex"), State->Transitions.Num() - 1);
		Result->SetStringField(TEXT("transitionId"), Trans.ID.ToString());
		return SuccessJson(Result);
	}

	FString AddTransitionCondition(const TSharedPtr<FJsonObject>& Args)
	{
		FStateTreeContext Ctx;
		if (!OpenContext(Args, Ctx)) { return ErrorJson(Ctx.Error); }
		FString StateName;
		Args->TryGetStringField(TEXT("state"), StateName);
		UStateTreeState* State = ResolveState(Ctx.EditorData, StateName);
		if (!State)
		{
			return ErrorJson(FString::Printf(TEXT("State '%s' not found."), *StateName));
		}
		double IndexNum = -1.0;
		Args->TryGetNumberField(TEXT("transitionIndex"), IndexNum);
		const int32 Index = static_cast<int32>(IndexNum);
		if (!State->Transitions.IsValidIndex(Index))
		{
			return ErrorJson(FString::Printf(TEXT("transitionIndex %d out of range (state has %d)."), Index, State->Transitions.Num()));
		}
		FString StructType;
		Args->TryGetStringField(TEXT("structType"), StructType);
		const TSharedPtr<FJsonObject>* InstPtr = nullptr;
		Args->TryGetObjectField(TEXT("instance"), InstPtr);
		const TSharedPtr<FJsonObject> Inst = InstPtr ? *InstPtr : nullptr;

		State->Modify();
		FStateTreeEditorNode* NewNode = nullptr;
		FString Err;
		if (!AddEditorNode(State->Transitions[Index].Conditions, StructType, FStateTreeConditionBase::StaticStruct(), Inst, NewNode, Err))
		{
			return ErrorJson(Err);
		}
		Save(Ctx.ST);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Ctx.ST->GetPathName());
		Result->SetNumberField(TEXT("transitionIndex"), Index);
		Result->SetStringField(TEXT("structType"), StructType);
		return SuccessJson(Result);
	}

	FString AddStateParameter(const TSharedPtr<FJsonObject>& Args)
	{
		FStateTreeContext Ctx;
		if (!OpenContext(Args, Ctx)) { return ErrorJson(Ctx.Error); }
		FString StateName;
		Args->TryGetStringField(TEXT("state"), StateName);
		UStateTreeState* State = ResolveState(Ctx.EditorData, StateName);
		if (!State)
		{
			return ErrorJson(FString::Printf(TEXT("State '%s' not found."), *StateName));
		}
		FString ParamName, ParamType;
		Args->TryGetStringField(TEXT("paramName"), ParamName);
		Args->TryGetStringField(TEXT("paramType"), ParamType);
		if (ParamName.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'paramName'."));
		}
		EPropertyBagPropertyType BagType;
		const FString T = ParamType.ToLower();
		if (T == TEXT("bool")) { BagType = EPropertyBagPropertyType::Bool; }
		else if (T == TEXT("int32") || T == TEXT("int")) { BagType = EPropertyBagPropertyType::Int32; }
		else if (T == TEXT("int64")) { BagType = EPropertyBagPropertyType::Int64; }
		else if (T == TEXT("float")) { BagType = EPropertyBagPropertyType::Float; }
		else if (T == TEXT("double")) { BagType = EPropertyBagPropertyType::Double; }
		else if (T == TEXT("name")) { BagType = EPropertyBagPropertyType::Name; }
		else if (T == TEXT("string")) { BagType = EPropertyBagPropertyType::String; }
		else if (T == TEXT("text")) { BagType = EPropertyBagPropertyType::Text; }
		else { return ErrorJson(TEXT("paramType must be Bool/Int32/Int64/Float/Double/Name/String/Text.")); }

		State->Modify();
		TArray<FPropertyBagPropertyDesc> Descs;
		Descs.Add(FPropertyBagPropertyDesc(FName(*ParamName), BagType));
		State->Parameters.Parameters.AddProperties(Descs, /*bOverwrite*/false);
		Save(Ctx.ST);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Ctx.ST->GetPathName());
		Result->SetStringField(TEXT("state"), State->Name.ToString());
		Result->SetStringField(TEXT("paramName"), ParamName);
		Result->SetStringField(TEXT("paramType"), ParamType);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"add_state_tree_task","description":"Add a task node to a StateTree state. 'structType' is the task struct (e.g. /Script/StateTreeModule.StateTreeRunParallelStateTreeTask, or a project FStateTreeTaskBase struct). Optional 'instance' object sets the task's instance-data properties.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"state":{"type":"string","description":"State name; empty = first subtree."},"structType":{"type":"string"},"instance":{"type":"object"}},"required":["assetPath","structType"]},"annotations":{"title":"Add State Tree Task","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_evaluator","description":"Add an evaluator (FStateTreeEvaluatorBase struct) to the StateTree's editor data. Optional 'instance' properties.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"structType":{"type":"string"},"instance":{"type":"object"}},"required":["assetPath","structType"]},"annotations":{"title":"Add State Tree Evaluator","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_global_task","description":"Add a global task (FStateTreeTaskBase struct) to the StateTree's editor data. Optional 'instance' properties.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"structType":{"type":"string"},"instance":{"type":"object"}},"required":["assetPath","structType"]},"annotations":{"title":"Add State Tree Global Task","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_enter_condition","description":"Add an enter condition (FStateTreeConditionBase struct) to a state. Optional 'instance' properties.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"state":{"type":"string"},"structType":{"type":"string"},"instance":{"type":"object"}},"required":["assetPath","structType"]},"annotations":{"title":"Add State Tree Enter Condition","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_transition","description":"Add a transition from a state. trigger: OnStateCompleted|OnStateSucceeded|OnStateFailed|OnTick|OnEvent. type: Succeeded|Failed|GotoState|NextState|NextSelectableState. For GotoState pass 'targetState' (state name). Optional 'delayDuration'.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"state":{"type":"string"},"trigger":{"type":"string"},"type":{"type":"string"},"targetState":{"type":"string"},"delayDuration":{"type":"number"}},"required":["assetPath","state"]},"annotations":{"title":"Add State Tree Transition","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_transition_condition","description":"Add a condition (FStateTreeConditionBase struct) to a state's transition by index (from add_state_tree_transition).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"state":{"type":"string"},"transitionIndex":{"type":"number"},"structType":{"type":"string"},"instance":{"type":"object"}},"required":["assetPath","state","transitionIndex","structType"]},"annotations":{"title":"Add State Tree Transition Condition","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_state_parameter","description":"Add a typed parameter to a state's parameter bag. paramType: Bool/Int32/Int64/Float/Double/Name/String/Text.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"state":{"type":"string"},"paramName":{"type":"string"},"paramType":{"type":"string"}},"required":["assetPath","paramName","paramType"]},"annotations":{"title":"Add State Tree State Parameter","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("add_state_tree_task")) { OutResult = AddTask(Args); return true; }
	if (ToolName == TEXT("add_state_tree_evaluator")) { OutResult = AddEvaluator(Args); return true; }
	if (ToolName == TEXT("add_state_tree_global_task")) { OutResult = AddGlobalTask(Args); return true; }
	if (ToolName == TEXT("add_state_tree_enter_condition")) { OutResult = AddEnterCondition(Args); return true; }
	if (ToolName == TEXT("add_state_tree_transition")) { OutResult = AddTransition(Args); return true; }
	if (ToolName == TEXT("add_state_tree_transition_condition")) { OutResult = AddTransitionCondition(Args); return true; }
	if (ToolName == TEXT("add_state_tree_state_parameter")) { OutResult = AddStateParameter(Args); return true; }
	return false;
}
}
}
