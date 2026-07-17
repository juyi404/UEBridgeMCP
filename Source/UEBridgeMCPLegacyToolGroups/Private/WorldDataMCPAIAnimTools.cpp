#include "WorldDataMCPAIAnimTools.h"

#include "WorldDataMCPCommon.h"

#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace AIAnimTools
{
namespace
{
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
		return !OutName.IsEmpty() && !OutFolder.IsEmpty();
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

	void SaveAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	// ---- Animation blueprints -----------------------------------------------------------

	FString CreateAnimBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath, SkeletonPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		Args->TryGetStringField(TEXT("skeleton"), SkeletonPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		USkeleton* Skeleton = Cast<USkeleton>(LoadAssetObject(SkeletonPath));
		if (!Skeleton)
		{
			return ErrorJson(FString::Printf(TEXT("Skeleton '%s' not found (required)."), *SkeletonPath));
		}
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->TargetSkeleton = Skeleton;
		FString ParentName;
		if (Args->TryGetStringField(TEXT("parentClass"), ParentName) && !ParentName.IsEmpty())
		{
			if (UClass* ParentClass = FindObject<UClass>(nullptr, *ParentName))
			{
				if (ParentClass->IsChildOf(UAnimInstance::StaticClass()))
				{
					Factory->ParentClass = ParentClass;
				}
			}
		}
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(
			AssetTools().CreateAsset(Name, Folder, UAnimBlueprint::StaticClass(), Factory));
		if (!AnimBP)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create anim blueprint at %s."), *DestPath));
		}
		SaveAsset(AnimBP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), AnimBP->GetPathName());
		Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
		return SuccessJson(Result);
	}

	UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBP)
	{
		TArray<UEdGraph*> All;
		AnimBP->GetAllGraphs(All);
		for (UEdGraph* Graph : All)
		{
			if (Graph && Graph->GetName() == TEXT("AnimGraph"))
			{
				return Graph;
			}
		}
		return All.Num() > 0 ? All[0] : nullptr;
	}

	FString AddAnimBpStateMachine(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, MachineName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("name"), MachineName);
		if (MachineName.IsEmpty())
		{
			MachineName = TEXT("NewStateMachine");
		}
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(LoadAssetObject(AssetPath));
		if (!AnimBP)
		{
			return ErrorJson(FString::Printf(TEXT("Anim blueprint '%s' not found."), *AssetPath));
		}
		UEdGraph* AnimGraph = FindAnimGraph(AnimBP);
		if (!AnimGraph)
		{
			return ErrorJson(TEXT("AnimGraph not found on the anim blueprint."));
		}

		UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
		AnimGraph->Modify();
		AnimGraph->AddNode(SMNode, /*bFromUI*/false, /*bSelectNewNode*/false);
		SMNode->CreateNewGuid();
		SMNode->PostPlacedNewNode();
		SMNode->AllocateDefaultPins();
		SMNode->NodePosX = 200;
		SMNode->NodePosY = 0;
		if (SMNode->EditorStateMachineGraph)
		{
			SMNode->EditorStateMachineGraph->Rename(*MachineName, nullptr, REN_DontCreateRedirectors);
		}

		// Optionally seed named states.
		TArray<FString> AddedStates;
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (Args->TryGetArrayField(TEXT("states"), States) && States && SMNode->EditorStateMachineGraph)
		{
			UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph;
			int32 Col = 0;
			for (const TSharedPtr<FJsonValue>& StateValue : *States)
			{
				const FString StateName = StateValue->AsString();
				if (StateName.IsEmpty())
				{
					continue;
				}
				UAnimStateNode* StateNode = NewObject<UAnimStateNode>(SMGraph);
				SMGraph->AddNode(StateNode, false, false);
				StateNode->CreateNewGuid();
				StateNode->PostPlacedNewNode();
				StateNode->AllocateDefaultPins();
				StateNode->NodePosX = 300 + (Col++ * 250);
				StateNode->NodePosY = 200;
				if (StateNode->BoundGraph)
				{
					StateNode->BoundGraph->Rename(*StateName, nullptr, REN_DontCreateRedirectors);
				}
				AddedStates.Add(StateName);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		SaveAsset(AnimBP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), AnimBP->GetPathName());
		Result->SetStringField(TEXT("stateMachine"),
			SMNode->EditorStateMachineGraph ? SMNode->EditorStateMachineGraph->GetName() : MachineName);
		Result->SetNumberField(TEXT("statesAdded"), AddedStates.Num());
		return SuccessJson(Result);
	}

	FString ReadAnimBlueprint(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(LoadAssetObject(AssetPath));
		if (!AnimBP)
		{
			return ErrorJson(FString::Printf(TEXT("Anim blueprint '%s' not found."), *AssetPath));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), AnimBP->GetPathName());
		Result->SetStringField(TEXT("skeleton"), AnimBP->TargetSkeleton ? AnimBP->TargetSkeleton->GetPathName() : FString());
		Result->SetStringField(TEXT("parentClass"), AnimBP->ParentClass ? AnimBP->ParentClass->GetName() : FString());

		TArray<TSharedPtr<FJsonValue>> Machines;
		TArray<UEdGraph*> All;
		AnimBP->GetAllGraphs(All);
		for (UEdGraph* Graph : All)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_StateMachine* SM = Cast<UAnimGraphNode_StateMachine>(Node))
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"),
						SM->EditorStateMachineGraph ? SM->EditorStateMachineGraph->GetName() : Node->GetName());
					int32 StateCount = 0;
					if (SM->EditorStateMachineGraph)
					{
						for (UEdGraphNode* SubNode : SM->EditorStateMachineGraph->Nodes)
						{
							if (Cast<UAnimStateNode>(SubNode))
							{
								++StateCount;
							}
						}
					}
					Entry->SetNumberField(TEXT("stateCount"), StateCount);
					Machines.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
		Result->SetArrayField(TEXT("stateMachines"), Machines);
		return SuccessJson(Result);
	}

	// ---- Behavior Tree / Blackboard -----------------------------------------------------

	FString CreateClassAsset(const TSharedPtr<FJsonObject>& Args, UClass* Class, const FString& FriendlyType)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UObject* Asset = AssetTools().CreateAsset(Name, Folder, Class, nullptr);
		if (!Asset)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create %s at %s."), *FriendlyType, *DestPath));
		}
		SaveAsset(Asset);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
		Result->SetStringField(TEXT("type"), FriendlyType);
		return SuccessJson(Result);
	}

	FString CreateBehaviorTree(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateClassAsset(Args, UBehaviorTree::StaticClass(), TEXT("BehaviorTree"));
	}

	FString CreateBlackboard(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateClassAsset(Args, UBlackboardData::StaticClass(), TEXT("BlackboardData"));
	}

	UBlackboardKeyType* MakeKeyType(UBlackboardData* Owner, const FString& Type)
	{
		const FString T = Type.ToLower();
		if (T == TEXT("bool")) { return NewObject<UBlackboardKeyType_Bool>(Owner); }
		if (T == TEXT("int") || T == TEXT("int32")) { return NewObject<UBlackboardKeyType_Int>(Owner); }
		if (T == TEXT("float")) { return NewObject<UBlackboardKeyType_Float>(Owner); }
		if (T == TEXT("string")) { return NewObject<UBlackboardKeyType_String>(Owner); }
		if (T == TEXT("name")) { return NewObject<UBlackboardKeyType_Name>(Owner); }
		if (T == TEXT("vector")) { return NewObject<UBlackboardKeyType_Vector>(Owner); }
		if (T == TEXT("rotator")) { return NewObject<UBlackboardKeyType_Rotator>(Owner); }
		if (T == TEXT("object")) { return NewObject<UBlackboardKeyType_Object>(Owner); }
		return nullptr;
	}

	FString AddBlackboardKey(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, KeyName, KeyType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("keyName"), KeyName);
		Args->TryGetStringField(TEXT("keyType"), KeyType);
		if (KeyName.IsEmpty() || KeyType.IsEmpty())
		{
			return ErrorJson(TEXT("'keyName' and 'keyType' (bool/int/float/string/name/vector/rotator/object) are required."));
		}
		UBlackboardData* Blackboard = Cast<UBlackboardData>(LoadAssetObject(AssetPath));
		if (!Blackboard)
		{
			return ErrorJson(FString::Printf(TEXT("Blackboard '%s' not found."), *AssetPath));
		}
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			if (Entry.EntryName == FName(*KeyName))
			{
				return ErrorJson(FString::Printf(TEXT("Key '%s' already exists."), *KeyName));
			}
		}
		UBlackboardKeyType* KeyTypeInstance = MakeKeyType(Blackboard, KeyType);
		if (!KeyTypeInstance)
		{
			return ErrorJson(FString::Printf(TEXT("Unsupported key type '%s'."), *KeyType));
		}
		Blackboard->Modify();
		FBlackboardEntry NewEntry;
		NewEntry.EntryName = FName(*KeyName);
		NewEntry.KeyType = KeyTypeInstance;
		Blackboard->Keys.Add(NewEntry);
		Blackboard->UpdateKeyIDs();
		SaveAsset(Blackboard);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blackboard->GetPathName());
		Result->SetStringField(TEXT("keyName"), KeyName);
		Result->SetStringField(TEXT("keyType"), KeyType);
		Result->SetNumberField(TEXT("totalKeys"), Blackboard->Keys.Num());
		return SuccessJson(Result);
	}

	FString ReadBlackboard(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlackboardData* Blackboard = Cast<UBlackboardData>(LoadAssetObject(AssetPath));
		if (!Blackboard)
		{
			return ErrorJson(FString::Printf(TEXT("Blackboard '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
			KeyJson->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			KeyJson->SetStringField(TEXT("type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : FString());
			Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blackboard->GetPathName());
		Result->SetStringField(TEXT("parent"), Blackboard->Parent ? Blackboard->Parent->GetPathName() : FString());
		Result->SetNumberField(TEXT("keyCount"), Keys.Num());
		Result->SetArrayField(TEXT("keys"), Keys);
		return SuccessJson(Result);
	}

	TSharedPtr<FJsonObject> WalkBTNode(UBTNode* Node)
	{
		if (!Node)
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Node->NodeName.IsEmpty() ? Node->GetName() : Node->NodeName);
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				UBTNode* ChildNode = Child.ChildComposite
					? static_cast<UBTNode*>(Child.ChildComposite)
					: static_cast<UBTNode*>(Child.ChildTask);
				if (TSharedPtr<FJsonObject> ChildJson = WalkBTNode(ChildNode))
				{
					Children.Add(MakeShared<FJsonValueObject>(ChildJson.ToSharedRef()));
				}
			}
			Entry->SetArrayField(TEXT("children"), Children);
		}
		return Entry;
	}

	FString ReadBehaviorTree(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBehaviorTree* BT = Cast<UBehaviorTree>(LoadAssetObject(AssetPath));
		if (!BT)
		{
			return ErrorJson(FString::Printf(TEXT("Behavior tree '%s' not found."), *AssetPath));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), BT->GetPathName());
		Result->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : FString());
		if (TSharedPtr<FJsonObject> Root = WalkBTNode(BT->RootNode))
		{
			Result->SetObjectField(TEXT("root"), Root);
		}
		return SuccessJson(Result);
	}

	// ---- StateTree ----------------------------------------------------------------------

	FString CreateStateTree(const TSharedPtr<FJsonObject>& Args)
	{
		return CreateClassAsset(Args, UStateTree::StaticClass(), TEXT("StateTree"));
	}

	UStateTreeEditorData* GetStateTreeEditorData(UStateTree* StateTree)
	{
		return StateTree ? Cast<UStateTreeEditorData>(StateTree->EditorData) : nullptr;
	}

	FString AddStateTreeState(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, StateName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("name"), StateName);
		if (StateName.IsEmpty())
		{
			StateName = TEXT("NewState");
		}
		UStateTree* StateTree = Cast<UStateTree>(LoadAssetObject(AssetPath));
		if (!StateTree)
		{
			return ErrorJson(FString::Printf(TEXT("StateTree '%s' not found."), *AssetPath));
		}
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(StateTree);
		if (!EditorData)
		{
			return ErrorJson(TEXT("StateTree has no editor data."));
		}

		FString ParentName;
		Args->TryGetStringField(TEXT("parent"), ParentName);
		EditorData->Modify();
		UStateTreeState* NewState = nullptr;
		if (!ParentName.IsEmpty())
		{
			// Find the parent state by name across all subtrees (shallow search).
			UStateTreeState* Parent = nullptr;
			TArray<UStateTreeState*> Stack;
			for (UStateTreeState* Sub : EditorData->SubTrees)
			{
				Stack.Add(Sub);
			}
			while (Stack.Num() > 0 && !Parent)
			{
				UStateTreeState* Cur = Stack.Pop();
				if (Cur && Cur->Name == FName(*ParentName))
				{
					Parent = Cur;
					break;
				}
				if (Cur)
				{
					for (UStateTreeState* Child : Cur->Children)
					{
						Stack.Add(Child);
					}
				}
			}
			if (!Parent)
			{
				return ErrorJson(FString::Printf(TEXT("Parent state '%s' not found."), *ParentName));
			}
			Parent->Modify();
			NewState = &Parent->AddChildState(FName(*StateName), EStateTreeStateType::State);
		}
		else
		{
			NewState = &EditorData->AddSubTree(FName(*StateName));
		}
		SaveAsset(StateTree);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), StateTree->GetPathName());
		Result->SetStringField(TEXT("state"), StateName);
		if (NewState)
		{
			Result->SetStringField(TEXT("stateId"), NewState->ID.ToString());
		}
		return SuccessJson(Result);
	}

	TSharedPtr<FJsonObject> WalkStateTreeState(UStateTreeState* State)
	{
		if (!State)
		{
			return nullptr;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), State->Name.ToString());
		Entry->SetStringField(TEXT("id"), State->ID.ToString());
		Entry->SetNumberField(TEXT("type"), static_cast<int32>(State->Type));
		TArray<TSharedPtr<FJsonValue>> Children;
		for (UStateTreeState* Child : State->Children)
		{
			if (TSharedPtr<FJsonObject> ChildJson = WalkStateTreeState(Child))
			{
				Children.Add(MakeShared<FJsonValueObject>(ChildJson.ToSharedRef()));
			}
		}
		Entry->SetArrayField(TEXT("children"), Children);
		return Entry;
	}

	FString ReadStateTree(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UStateTree* StateTree = Cast<UStateTree>(LoadAssetObject(AssetPath));
		if (!StateTree)
		{
			return ErrorJson(FString::Printf(TEXT("StateTree '%s' not found."), *AssetPath));
		}
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(StateTree);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), StateTree->GetPathName());
		if (EditorData)
		{
			Result->SetStringField(TEXT("schema"),
				EditorData->Schema ? EditorData->Schema->GetClass()->GetName() : FString(TEXT("(none)")));
			TArray<TSharedPtr<FJsonValue>> SubTrees;
			for (UStateTreeState* Sub : EditorData->SubTrees)
			{
				if (TSharedPtr<FJsonObject> SubJson = WalkStateTreeState(Sub))
				{
					SubTrees.Add(MakeShared<FJsonValueObject>(SubJson.ToSharedRef()));
				}
			}
			Result->SetArrayField(TEXT("subTrees"), SubTrees);
		}
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_anim_blueprint","description":"Create an Animation Blueprint for a target Skeleton. Optional parentClass (must derive from AnimInstance).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Anim/ABP_Foo."},"skeleton":{"type":"string","description":"Skeleton asset path (required)."},"parentClass":{"type":"string"}},"required":["destPath","skeleton"]},"annotations":{"title":"Create Anim Blueprint","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_anim_bp_state_machine","description":"Add a state machine to an Animation Blueprint's AnimGraph. Optionally seed 'states' (array of names). Compiles and saves.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"},"states":{"type":"array","items":{"type":"string"}}},"required":["assetPath"]},"annotations":{"title":"Add Anim BP State Machine","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_anim_blueprint","description":"Read an Animation Blueprint: target skeleton, parent class, and its state machines with state counts.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Anim Blueprint","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_behavior_tree","description":"Create a Behavior Tree asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/AI/BT_Foo."}},"required":["destPath"]},"annotations":{"title":"Create Behavior Tree","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"create_blackboard","description":"Create a Blackboard Data asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/AI/BB_Foo."}},"required":["destPath"]},"annotations":{"title":"Create Blackboard","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_blackboard_key","description":"Add a key to a Blackboard. keyType: bool, int, float, string, name, vector, rotator, object.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"keyName":{"type":"string"},"keyType":{"type":"string"}},"required":["assetPath","keyName","keyType"]},"annotations":{"title":"Add Blackboard Key","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_blackboard","description":"Read a Blackboard's keys (name and key-type class) and parent.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Blackboard","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_behavior_tree","description":"Read a Behavior Tree: its blackboard and the node hierarchy (class/name, recursive children).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read Behavior Tree","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_state_tree","description":"Create a StateTree asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/AI/ST_Foo."}},"required":["destPath"]},"annotations":{"title":"Create State Tree","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_state_tree_state","description":"Add a state to a StateTree. With 'parent' (a state name) adds a child state, otherwise adds a root subtree.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"name":{"type":"string"},"parent":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Add State Tree State","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_state_tree","description":"Read a StateTree: schema and its state hierarchy (name/id/type, recursive children).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read State Tree","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_anim_blueprint")) { OutResult = CreateAnimBlueprint(Args); return true; }
	if (ToolName == TEXT("add_anim_bp_state_machine")) { OutResult = AddAnimBpStateMachine(Args); return true; }
	if (ToolName == TEXT("read_anim_blueprint")) { OutResult = ReadAnimBlueprint(Args); return true; }

	if (ToolName == TEXT("create_behavior_tree")) { OutResult = CreateBehaviorTree(Args); return true; }
	if (ToolName == TEXT("create_blackboard")) { OutResult = CreateBlackboard(Args); return true; }
	if (ToolName == TEXT("add_blackboard_key")) { OutResult = AddBlackboardKey(Args); return true; }
	if (ToolName == TEXT("read_blackboard")) { OutResult = ReadBlackboard(Args); return true; }
	if (ToolName == TEXT("read_behavior_tree")) { OutResult = ReadBehaviorTree(Args); return true; }

	if (ToolName == TEXT("create_state_tree")) { OutResult = CreateStateTree(Args); return true; }
	if (ToolName == TEXT("add_state_tree_state")) { OutResult = AddStateTreeState(Args); return true; }
	if (ToolName == TEXT("read_state_tree")) { OutResult = ReadStateTree(Args); return true; }

	return false;
}
}
}
