#include "WorldDataMCPGraphTools.h"

#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "SceneTypes.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace GraphTools
{
namespace
{
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
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ClassName)
			{
				return *It;
			}
		}
		return nullptr;
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

	// ---- Blueprint graph ----------------------------------------------------------------

	UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		TArray<UEdGraph*> All;
		Blueprint->GetAllGraphs(All);
		if (GraphName.IsEmpty())
		{
			if (Blueprint->UbergraphPages.Num() > 0)
			{
				return Blueprint->UbergraphPages[0];
			}
			return All.Num() > 0 ? All[0] : nullptr;
		}
		for (UEdGraph* Graph : All)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId)
	{
		FGuid Guid;
		if (FGuid::Parse(NodeId, Guid))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == Guid)
				{
					return Node;
				}
			}
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (Node->GetName() == NodeId
				|| Node->GetNodeTitle(ENodeTitleType::ListView).ToString() == NodeId)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UFunction* ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName, const FString& TargetClassName)
	{
		UFunction* Found = nullptr;
		if (!TargetClassName.IsEmpty())
		{
			if (UClass* TargetClass = ResolveClass(TargetClassName))
			{
				Found = TargetClass->FindFunctionByName(FName(*FunctionName));
			}
		}
		if (!Found && Blueprint->GeneratedClass)
		{
			Found = Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionName));
		}
		if (!Found && Blueprint->ParentClass)
		{
			Found = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));
		}
		if (!Found)
		{
			UClass* Libraries[] = {
				UGameplayStatics::StaticClass(),
				UKismetSystemLibrary::StaticClass(),
				UKismetMathLibrary::StaticClass(),
				UKismetStringLibrary::StaticClass()
			};
			for (UClass* Lib : Libraries)
			{
				Found = Lib->FindFunctionByName(FName(*FunctionName));
				if (Found)
				{
					break;
				}
			}
		}
		return Found;
	}

	TSharedRef<FJsonObject> MakeNodeJson(UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
		Entry->SetStringField(TEXT("name"), Node->GetName());
		Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinJson->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		Entry->SetArrayField(TEXT("pins"), Pins);
		return Entry;
	}

	FString AddBlueprintNode(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		FString GraphName;
		Args->TryGetStringField(TEXT("graph"), GraphName);
		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return ErrorJson(TEXT("Target graph not found."));
		}

		FString NodeType = TEXT("callFunction");
		Args->TryGetStringField(TEXT("nodeType"), NodeType);

		double PosX = 0.0, PosY = 0.0;
		Args->TryGetNumberField(TEXT("posX"), PosX);
		Args->TryGetNumberField(TEXT("posY"), PosY);

		UEdGraphNode* NewNode = nullptr;
		if (NodeType.Equals(TEXT("callFunction"), ESearchCase::IgnoreCase))
		{
			FString FunctionName, TargetClassName;
			Args->TryGetStringField(TEXT("function"), FunctionName);
			Args->TryGetStringField(TEXT("targetClass"), TargetClassName);
			UFunction* Function = ResolveFunction(Blueprint, FunctionName, TargetClassName);
			if (!Function)
			{
				return ErrorJson(FString::Printf(TEXT("Function '%s' not found."), *FunctionName));
			}
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
			Node->SetFromFunction(Function);
			NewNode = Node;
		}
		else if (NodeType.Equals(TEXT("event"), ESearchCase::IgnoreCase))
		{
			FString EventName;
			Args->TryGetStringField(TEXT("event"), EventName);
			UClass* EventClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
			UFunction* EventFunc = EventClass ? EventClass->FindFunctionByName(FName(*EventName)) : nullptr;
			if (!EventFunc)
			{
				return ErrorJson(FString::Printf(TEXT("Event '%s' not found on the blueprint class."), *EventName));
			}
			UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
			Node->EventReference.SetFromField<UFunction>(EventFunc, /*bIsConsideredSelfContext*/true);
			Node->bOverrideFunction = true;
			NewNode = Node;
		}
		else if (NodeType.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("variableSet"), ESearchCase::IgnoreCase))
		{
			FString VarName;
			Args->TryGetStringField(TEXT("variable"), VarName);
			if (VarName.IsEmpty())
			{
				return ErrorJson(TEXT("'variable' is required for variableGet/variableSet."));
			}
			if (NodeType.Equals(TEXT("variableGet"), ESearchCase::IgnoreCase))
			{
				UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
				Node->VariableReference.SetSelfMember(FName(*VarName));
				NewNode = Node;
			}
			else
			{
				UK2Node_VariableSet* Node = NewObject<UK2Node_VariableSet>(Graph);
				Node->VariableReference.SetSelfMember(FName(*VarName));
				NewNode = Node;
			}
		}
		else
		{
			return ErrorJson(TEXT("'nodeType' must be callFunction, event, variableGet, or variableSet."));
		}

		Graph->Modify();
		Graph->AddNode(NewNode, /*bFromUI*/false, /*bSelectNewNode*/false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		NewNode->NodePosX = static_cast<int32>(PosX);
		NewNode->NodePosY = static_cast<int32>(PosY);
		if (UK2Node* AsK2 = Cast<UK2Node>(NewNode))
		{
			AsK2->ReconstructNode();
		}
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		SaveAsset(Blueprint);

		return SuccessJson(MakeNodeJson(NewNode));
	}

	FString ConnectBlueprintPins(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		FString GraphName;
		Args->TryGetStringField(TEXT("graph"), GraphName);
		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return ErrorJson(TEXT("Target graph not found."));
		}
		FString SourceNodeId, SourcePinName, TargetNodeId, TargetPinName;
		Args->TryGetStringField(TEXT("sourceNode"), SourceNodeId);
		Args->TryGetStringField(TEXT("sourcePin"), SourcePinName);
		Args->TryGetStringField(TEXT("targetNode"), TargetNodeId);
		Args->TryGetStringField(TEXT("targetPin"), TargetPinName);

		UEdGraphNode* SourceNode = FindNode(Graph, SourceNodeId);
		UEdGraphNode* TargetNode = FindNode(Graph, TargetNodeId);
		if (!SourceNode || !TargetNode)
		{
			return ErrorJson(TEXT("Source or target node not found."));
		}
		UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName), EGPD_Output);
		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName), EGPD_Input);
		if (!SourcePin || !TargetPin)
		{
			return ErrorJson(TEXT("Source output pin or target input pin not found."));
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		const bool bConnected = Schema && Schema->TryCreateConnection(SourcePin, TargetPin);
		if (!bConnected)
		{
			FString Reason;
			if (Schema)
			{
				Reason = Schema->CanCreateConnection(SourcePin, TargetPin).Message.ToString();
			}
			return ErrorJson(FString::Printf(TEXT("Connection refused: %s"), *Reason));
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		SaveAsset(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetBoolField(TEXT("connected"), true);
		return SuccessJson(Result);
	}

	FString FindBlueprintNodes(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		FString GraphName;
		Args->TryGetStringField(TEXT("graph"), GraphName);
		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return ErrorJson(TEXT("Target graph not found."));
		}
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(MakeShared<FJsonValueObject>(MakeNodeJson(Node)));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph"), Graph->GetName());
		Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
		Result->SetArrayField(TEXT("nodes"), Nodes);
		return SuccessJson(Result);
	}

	FString DeleteBlueprintNode(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadAssetObject(AssetPath));
		if (!Blueprint)
		{
			return ErrorJson(FString::Printf(TEXT("Blueprint '%s' not found."), *AssetPath));
		}
		FString GraphName, NodeId;
		Args->TryGetStringField(TEXT("graph"), GraphName);
		Args->TryGetStringField(TEXT("node"), NodeId);
		UEdGraph* Graph = ResolveGraph(Blueprint, GraphName);
		if (!Graph)
		{
			return ErrorJson(TEXT("Target graph not found."));
		}
		UEdGraphNode* Node = FindNode(Graph, NodeId);
		if (!Node)
		{
			return ErrorJson(FString::Printf(TEXT("Node '%s' not found."), *NodeId));
		}
		Node->BreakAllNodeLinks();
		Graph->RemoveNode(Node);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		SaveAsset(Blueprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
		Result->SetBoolField(TEXT("deleted"), true);
		return SuccessJson(Result);
	}

	// ---- Material graph -----------------------------------------------------------------

	UClass* ResolveMaterialExpressionClass(const FString& ExpressionType)
	{
		FString ClassName = ExpressionType;
		if (!ClassName.StartsWith(TEXT("MaterialExpression")) && !ClassName.StartsWith(TEXT("UMaterialExpression")))
		{
			ClassName = TEXT("MaterialExpression") + ClassName;
		}
		if (UClass* Direct = ResolveClass(ClassName))
		{
			if (Direct->IsChildOf(UMaterialExpression::StaticClass()))
			{
				return Direct;
			}
		}
		// Try the engine script path.
		FString Stripped = ClassName.StartsWith(TEXT("U")) ? ClassName.Mid(1) : ClassName;
		if (UClass* Scripted = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Stripped)))
		{
			if (Scripted->IsChildOf(UMaterialExpression::StaticClass()))
			{
				return Scripted;
			}
		}
		return nullptr;
	}

	UMaterialExpression* FindExpressionByName(UMaterial* Material, const FString& Name)
	{
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Material->GetExpressions())
		{
			UMaterialExpression* Expr = ExprPtr;
			if (Expr && (Expr->Desc == Name || Expr->GetName() == Name))
			{
				return Expr;
			}
		}
		return nullptr;
	}

	bool ParseMaterialProperty(const FString& Name, EMaterialProperty& Out)
	{
		const FString N = Name.ToLower();
		if (N == TEXT("basecolor")) { Out = MP_BaseColor; return true; }
		if (N == TEXT("metallic")) { Out = MP_Metallic; return true; }
		if (N == TEXT("specular")) { Out = MP_Specular; return true; }
		if (N == TEXT("roughness")) { Out = MP_Roughness; return true; }
		if (N == TEXT("emissivecolor") || N == TEXT("emissive")) { Out = MP_EmissiveColor; return true; }
		if (N == TEXT("opacity")) { Out = MP_Opacity; return true; }
		if (N == TEXT("opacitymask")) { Out = MP_OpacityMask; return true; }
		if (N == TEXT("normal")) { Out = MP_Normal; return true; }
		if (N == TEXT("ambientocclusion") || N == TEXT("ao")) { Out = MP_AmbientOcclusion; return true; }
		if (N == TEXT("worldpositionoffset") || N == TEXT("wpo")) { Out = MP_WorldPositionOffset; return true; }
		return false;
	}

	FString AddMaterialExpression(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, ExpressionType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("expressionType"), ExpressionType);
		UMaterial* Material = Cast<UMaterial>(LoadAssetObject(AssetPath));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Material '%s' not found."), *AssetPath));
		}
		UClass* ExprClass = ResolveMaterialExpressionClass(ExpressionType);
		if (!ExprClass)
		{
			return ErrorJson(FString::Printf(TEXT("Material expression type '%s' not found."), *ExpressionType));
		}
		double PosX = 0.0, PosY = 0.0;
		Args->TryGetNumberField(TEXT("posX"), PosX);
		Args->TryGetNumberField(TEXT("posY"), PosY);

		UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
			Material, ExprClass, static_cast<int32>(PosX), static_cast<int32>(PosY));
		if (!Expr)
		{
			return ErrorJson(TEXT("Failed to create material expression."));
		}

		FString ParameterName;
		if (Args->TryGetStringField(TEXT("parameterName"), ParameterName) && !ParameterName.IsEmpty())
		{
			if (UMaterialExpressionParameter* AsParam = Cast<UMaterialExpressionParameter>(Expr))
			{
				AsParam->ParameterName = FName(*ParameterName);
			}
			Expr->Desc = ParameterName;
		}
		double Scalar = 0.0;
		if (Args->TryGetNumberField(TEXT("scalar"), Scalar))
		{
			if (UMaterialExpressionScalarParameter* AsScalar = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				AsScalar->DefaultValue = static_cast<float>(Scalar);
			}
			else if (UMaterialExpressionConstant* AsConst = Cast<UMaterialExpressionConstant>(Expr))
			{
				AsConst->R = static_cast<float>(Scalar);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Vector = nullptr;
		if (Args->TryGetArrayField(TEXT("vector"), Vector) && Vector)
		{
			const TArray<TSharedPtr<FJsonValue>>& V = *Vector;
			const FLinearColor Color(
				V.Num() > 0 ? static_cast<float>(V[0]->AsNumber()) : 0.0f,
				V.Num() > 1 ? static_cast<float>(V[1]->AsNumber()) : 0.0f,
				V.Num() > 2 ? static_cast<float>(V[2]->AsNumber()) : 0.0f,
				V.Num() > 3 ? static_cast<float>(V[3]->AsNumber()) : 1.0f);
			if (UMaterialExpressionVectorParameter* AsVec = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				AsVec->DefaultValue = Color;
			}
			else if (UMaterialExpressionConstant3Vector* AsConst3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
			{
				AsConst3->Constant = Color;
			}
		}

		UMaterialEditingLibrary::RecompileMaterial(Material);
		SaveAsset(Material);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
		Result->SetStringField(TEXT("expression"), Expr->GetName());
		Result->SetStringField(TEXT("expressionClass"), ExprClass->GetName());
		return SuccessJson(Result);
	}

	FString ConnectMaterialExpression(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UMaterial* Material = Cast<UMaterial>(LoadAssetObject(AssetPath));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Material '%s' not found."), *AssetPath));
		}
		FString FromName, FromOutput, ToName, ToInput, PropertyName;
		Args->TryGetStringField(TEXT("fromExpression"), FromName);
		Args->TryGetStringField(TEXT("fromOutput"), FromOutput);
		Args->TryGetStringField(TEXT("toExpression"), ToName);
		Args->TryGetStringField(TEXT("toInput"), ToInput);
		Args->TryGetStringField(TEXT("property"), PropertyName);

		UMaterialExpression* FromExpr = FindExpressionByName(Material, FromName);
		if (!FromExpr)
		{
			return ErrorJson(FString::Printf(TEXT("Source expression '%s' not found."), *FromName));
		}

		bool bOk = false;
		if (!PropertyName.IsEmpty())
		{
			EMaterialProperty Property;
			if (!ParseMaterialProperty(PropertyName, Property))
			{
				return ErrorJson(FString::Printf(TEXT("Unknown material property '%s'."), *PropertyName));
			}
			bOk = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, FromOutput, Property);
		}
		else
		{
			UMaterialExpression* ToExpr = FindExpressionByName(Material, ToName);
			if (!ToExpr)
			{
				return ErrorJson(FString::Printf(TEXT("Target expression '%s' not found."), *ToName));
			}
			bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpr, FromOutput, ToExpr, ToInput);
		}
		if (!bOk)
		{
			return ErrorJson(TEXT("Connection failed (check output/input names)."));
		}
		UMaterialEditingLibrary::RecompileMaterial(Material);
		SaveAsset(Material);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
		Result->SetBoolField(TEXT("connected"), true);
		return SuccessJson(Result);
	}

	FString InspectMaterialGraph(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UMaterial* Material = Cast<UMaterial>(LoadAssetObject(AssetPath));
		if (!Material)
		{
			return ErrorJson(FString::Printf(TEXT("Material '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Expressions;
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Material->GetExpressions())
		{
			UMaterialExpression* Expr = ExprPtr;
			if (!Expr)
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Expr->GetName());
			Entry->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
			if (!Expr->Desc.IsEmpty())
			{
				Entry->SetStringField(TEXT("desc"), Expr->Desc);
			}
			Entry->SetNumberField(TEXT("posX"), Expr->MaterialExpressionEditorX);
			Entry->SetNumberField(TEXT("posY"), Expr->MaterialExpressionEditorY);
			Expressions.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
		Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());
		Result->SetArrayField(TEXT("expressions"), Expressions);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"add_blueprint_node","description":"Add a K2 node to a Blueprint graph. nodeType: callFunction (needs 'function', optional 'targetClass'), event (needs 'event'), variableGet/variableSet (needs 'variable'). Returns the node guid and pins. Compiles and saves.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"graph":{"type":"string","description":"Graph name; defaults to the event graph."},"nodeType":{"type":"string"},"function":{"type":"string"},"targetClass":{"type":"string"},"event":{"type":"string"},"variable":{"type":"string"},"posX":{"type":"number"},"posY":{"type":"number"}},"required":["assetPath"]},"annotations":{"title":"Add Blueprint Node","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"connect_blueprint_pins","description":"Connect a source node's output pin to a target node's input pin in a Blueprint graph. Nodes identified by guid (from add_blueprint_node/find_blueprint_nodes) or name/title.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"graph":{"type":"string"},"sourceNode":{"type":"string"},"sourcePin":{"type":"string"},"targetNode":{"type":"string"},"targetPin":{"type":"string"}},"required":["assetPath","sourceNode","sourcePin","targetNode","targetPin"]},"annotations":{"title":"Connect Blueprint Pins","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"find_blueprint_nodes","description":"List nodes in a Blueprint graph with their guid, title, class, and pins (name/direction/type).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"graph":{"type":"string","description":"Graph name; defaults to the event graph."}},"required":["assetPath"]},"annotations":{"title":"Find Blueprint Nodes","readOnlyHint":true,"openWorldHint":false}},
{"name":"delete_blueprint_node","description":"Delete a node from a Blueprint graph by guid or name, breaking its links first.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"graph":{"type":"string"},"node":{"type":"string"}},"required":["assetPath","node"]},"annotations":{"title":"Delete Blueprint Node","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"add_material_expression","description":"Add a material expression node (e.g. ScalarParameter, VectorParameter, Constant, Constant3Vector, TextureSample, Multiply, Add, LinearInterpolate). Optionally set parameterName, scalar, or vector [r,g,b,a]. Recompiles and saves.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"expressionType":{"type":"string"},"parameterName":{"type":"string"},"scalar":{"type":"number"},"vector":{"type":"array","items":{"type":"number"}},"posX":{"type":"number"},"posY":{"type":"number"}},"required":["assetPath","expressionType"]},"annotations":{"title":"Add Material Expression","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"connect_material_expression","description":"Connect a material expression output to another expression's input, or to a material property (BaseColor, Metallic, Roughness, EmissiveColor, Normal, Opacity, AmbientOcclusion, WorldPositionOffset). Expressions identified by their Desc/name. Provide 'property' for a property connection, else 'toExpression'+'toInput'.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"fromExpression":{"type":"string"},"fromOutput":{"type":"string","description":"Output name; empty for the default output."},"toExpression":{"type":"string"},"toInput":{"type":"string"},"property":{"type":"string"}},"required":["assetPath","fromExpression"]},"annotations":{"title":"Connect Material Expression","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"inspect_material_graph","description":"List a material's expression nodes with class, description, and editor position.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Inspect Material Graph","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("add_blueprint_node")) { OutResult = AddBlueprintNode(Args); return true; }
	if (ToolName == TEXT("connect_blueprint_pins")) { OutResult = ConnectBlueprintPins(Args); return true; }
	if (ToolName == TEXT("find_blueprint_nodes")) { OutResult = FindBlueprintNodes(Args); return true; }
	if (ToolName == TEXT("delete_blueprint_node")) { OutResult = DeleteBlueprintNode(Args); return true; }

	if (ToolName == TEXT("add_material_expression")) { OutResult = AddMaterialExpression(Args); return true; }
	if (ToolName == TEXT("connect_material_expression")) { OutResult = ConnectMaterialExpression(Args); return true; }
	if (ToolName == TEXT("inspect_material_graph")) { OutResult = InspectMaterialGraph(Args); return true; }

	return false;
}
}
}
