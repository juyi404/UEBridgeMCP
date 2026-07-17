#include "WorldDataMCPPCGTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataSceneBriefStore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "Misc/PackageName.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGVolume.h"
#include "ScopedTransaction.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "WorldDataMCPPCGTools"

namespace WorldDataMCP
{
namespace PCGTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return GWorld;
	}

	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	void SaveLoadedAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

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
			OutError = TEXT("destPath must be a content path like /Game/PCG/PCG_Forest.");
			return false;
		}
		OutName = FPackageName::GetShortName(Path);
		OutFolder = FPackageName::GetLongPackagePath(Path);
		return !OutName.IsEmpty() && !OutFolder.IsEmpty();
	}

	UPCGGraph* LoadGraph(const FString& Path)
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
		return LoadObject<UPCGGraph>(nullptr, *Normalized);
	}

	// Find a node by its internal name, including the implicit input/output nodes.
	UPCGNode* FindNode(UPCGGraph* Graph, const FString& NodeName)
	{
		if (!Graph || NodeName.IsEmpty())
		{
			return nullptr;
		}
		for (const TObjectPtr<UPCGNode>& Node : Graph->GetNodes())
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Node;
			}
		}
		if (UPCGNode* In = Graph->GetInputNode())
		{
			if (In->GetName() == NodeName) { return In; }
		}
		if (UPCGNode* Out = Graph->GetOutputNode())
		{
			if (Out->GetName() == NodeName) { return Out; }
		}
		return nullptr;
	}

	// Resolve a PCG settings UClass from a friendly/short name, e.g. "SurfaceSampler",
	// "PCGSurfaceSamplerSettings", or a full /Script path. Returns nullptr if not a UPCGSettings.
	UClass* ResolvePCGSettingsClass(const FString& Name)
	{
		auto Validate = [](UClass* C) -> UClass* { return (C && C->IsChildOf(UPCGSettings::StaticClass())) ? C : nullptr; };

		if (UClass* Direct = Validate(FindObject<UClass>(nullptr, *Name)))
		{
			return Direct;
		}
		// Try /Script/PCG. with the given name, with a UPCG...Settings normalisation.
		TArray<FString> Candidates;
		Candidates.Add(Name);
		FString Clean = Name;
		Clean.RemoveFromStart(TEXT("U"));
		Candidates.Add(Clean);
		if (!Clean.StartsWith(TEXT("PCG"))) { Candidates.Add(FString::Printf(TEXT("PCG%s"), *Clean)); }
		if (!Clean.EndsWith(TEXT("Settings"))) { Candidates.Add(FString::Printf(TEXT("PCG%sSettings"), *Clean)); }
		Candidates.Add(FString::Printf(TEXT("U%s"), *Clean));
		for (const FString& Cand : Candidates)
		{
			if (UClass* C = Validate(FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/PCG.%s"), *Cand))))
			{
				return C;
			}
		}
		// Last resort: scan by short name.
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UPCGSettings::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract))
			{
				const FString N = It->GetName();
				if (N == Name || N == Clean || N == FString::Printf(TEXT("PCG%sSettings"), *Clean))
				{
					return *It;
				}
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> NodeJson(UPCGNode* Node)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Node->GetName());
		Entry->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		if (const UPCGSettings* S = Node->GetSettings())
		{
			Entry->SetStringField(TEXT("settingsClass"), S->GetClass()->GetName());
		}
		TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
		for (const TObjectPtr<UPCGPin>& P : Node->GetInputPins())
		{
			if (P) { InPins.Add(MakeShared<FJsonValueString>(P->Properties.Label.ToString())); }
		}
		for (const TObjectPtr<UPCGPin>& P : Node->GetOutputPins())
		{
			if (P) { OutPins.Add(MakeShared<FJsonValueString>(P->Properties.Label.ToString())); }
		}
		Entry->SetArrayField(TEXT("inputPins"), InPins);
		Entry->SetArrayField(TEXT("outputPins"), OutPins);
		return Entry;
	}

	// Resolve a (possibly dotted) property path like "SamplerParams.Mode" against a struct/object,
	// descending through nested struct properties and object-pointer properties (into the pointed-to
	// object). Returns the final FProperty and the address of its value, or false if any segment is
	// unresolvable. A single-segment path behaves exactly like a top-level FindPropertyByName.
	static bool ResolvePropertyPath(UStruct* OwnerStruct, void* Container, const FString& Path,
		FProperty*& OutProperty, void*& OutAddr)
	{
		OutProperty = nullptr;
		OutAddr = nullptr;
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), /*CullEmpty*/ true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurStruct = OwnerStruct;
		void* CurContainer = Container;
		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			if (!CurStruct || !CurContainer)
			{
				return false;
			}
			FProperty* Property = CurStruct->FindPropertyByName(FName(*Segments[i]));
			if (!Property)
			{
				return false;
			}
			if (i == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutAddr = Property->ContainerPtrToValuePtr<void>(CurContainer);
				return true;
			}
			// Intermediate segment: descend into a nested struct or a pointed-to object.
			if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
			{
				CurContainer = StructProp->ContainerPtrToValuePtr<void>(CurContainer);
				CurStruct = StructProp->Struct;
				continue;
			}
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
			{
				UObject* Inner = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CurContainer));
				if (!Inner)
				{
					return false;
				}
				CurContainer = Inner;
				CurStruct = Inner->GetClass();
				continue;
			}
			return false; // cannot descend through a non-struct, non-object property
		}
		return false;
	}

	// Apply a {name: value} map of properties onto a UPCGSettings via reflection. Keys may be dotted
	// paths (e.g. "SamplerParams.Mode" / "SamplerParams.DistanceIncrement") that descend through
	// nested structs and object pointers; object/class-ref properties accept an asset/class path.
	void ApplyProperties(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutApplied, TArray<FString>& OutFailed)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			FProperty* Property = nullptr;
			void* Addr = nullptr;
			if (!ResolvePropertyPath(Settings->GetClass(), Settings, Pair.Key, Property, Addr) || !Property || !Addr)
			{
				OutFailed.Add(Pair.Key);
				continue;
			}

			// Object/class reference properties take an asset/class path string.
			if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
			{
				const FString PathStr = Pair.Value->AsString();
				UObject* Loaded = PathStr.IsEmpty() ? nullptr : StaticLoadObject(ObjProp->PropertyClass, nullptr, *PathStr);
				if (!PathStr.IsEmpty() && !Loaded)
				{
					OutFailed.Add(Pair.Key);
					continue;
				}
				ObjProp->SetObjectPropertyValue(Addr, Loaded);
				OutApplied.Add(Pair.Key);
				continue;
			}

			FString TextValue;
			switch (Pair.Value->Type)
			{
			case EJson::Number: TextValue = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
			case EJson::Boolean: TextValue = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
			default: TextValue = Pair.Value->AsString(); break;
			}
			if (Property->ImportText_Direct(*TextValue, Addr, Settings, PPF_None) != nullptr)
			{
				OutApplied.Add(Pair.Key);
			}
			else
			{
				OutFailed.Add(Pair.Key);
			}
		}
	}

	UPCGComponent* FindPCGComponent(UWorld* World, const FString& ActorLabel, AActor*& OutActor)
	{
		OutActor = nullptr;
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (ActorLabel.IsEmpty() || Actor->GetActorLabel() == ActorLabel || Actor->GetName() == ActorLabel)
			{
				if (UPCGComponent* Comp = Actor->FindComponentByClass<UPCGComponent>())
				{
					OutActor = Actor;
					return Comp;
				}
				if (!ActorLabel.IsEmpty())
				{
					return nullptr; // matched the actor but it has no PCG component
				}
			}
		}
		return nullptr;
	}

	// ---- tools ---------------------------------------------------------------------------

	FString ListPCGGraphs(const TSharedPtr<FJsonObject>& Args)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/PCG"), TEXT("PCGGraph")), Assets, true);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FAssetData& A : Assets)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), A.AssetName.ToString());
			E->SetStringField(TEXT("path"), A.GetObjectPathString());
			Out.Add(MakeShared<FJsonValueObject>(E));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), Out.Num());
		Result->SetArrayField(TEXT("graphs"), Out);
		return SuccessJson(Result);
	}

	FString CreatePCGGraph(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UPCGGraph* Graph = Cast<UPCGGraph>(AssetTools().CreateAsset(Name, Folder, UPCGGraph::StaticClass(), nullptr));
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create PCG graph at %s."), *DestPath));
		}
		SaveLoadedAsset(Graph);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		return SuccessJson(Result);
	}

	FString ReadPCGGraph(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		TArray<TSharedPtr<FJsonValue>> Nodes, Edges;
		for (const TObjectPtr<UPCGNode>& Node : Graph->GetNodes())
		{
			if (!Node) { continue; }
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node)));
			// Edges from this node's output pins.
			for (const TObjectPtr<UPCGPin>& OutPin : Node->GetOutputPins())
			{
				if (!OutPin) { continue; }
				for (const TObjectPtr<UPCGEdge>& Edge : OutPin->Edges)
				{
					if (!Edge) { continue; }
					UPCGPin* OtherPin = (Edge->InputPin == OutPin) ? Edge->OutputPin.Get() : Edge->InputPin.Get();
					if (OtherPin && OtherPin->Node.Get())
					{
						TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
						E->SetStringField(TEXT("from"), Node->GetName());
						E->SetStringField(TEXT("fromPin"), OutPin->Properties.Label.ToString());
						E->SetStringField(TEXT("to"), OtherPin->Node->GetName());
						E->SetStringField(TEXT("toPin"), OtherPin->Properties.Label.ToString());
						Edges.Add(MakeShared<FJsonValueObject>(E));
					}
				}
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
		Result->SetArrayField(TEXT("nodes"), Nodes);
		Result->SetArrayField(TEXT("edges"), Edges);
		if (UPCGNode* In = Graph->GetInputNode()) { Result->SetStringField(TEXT("inputNode"), In->GetName()); }
		if (UPCGNode* Out = Graph->GetOutputNode()) { Result->SetStringField(TEXT("outputNode"), Out->GetName()); }
		return SuccessJson(Result);
	}

	FString AddPCGNode(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NodeType;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("nodeType"), NodeType);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UClass* SettingsClass = ResolvePCGSettingsClass(NodeType);
		if (!SettingsClass)
		{
			return ErrorJson(FString::Printf(TEXT("PCG settings class '%s' not found (try e.g. SurfaceSampler, TransformPoints, DensityFilter, StaticMeshSpawner, Difference, SplineSampler)."), *NodeType));
		}

		FScopedTransaction Transaction(LOCTEXT("AddPCGNode", "Add PCG Node"));
		Graph->Modify();
		UPCGSettings* NewSettings = NewObject<UPCGSettings>(Graph, SettingsClass, NAME_None, RF_Transactional);
		UPCGNode* NewNode = Graph->AddNodeInstance(NewSettings);
		if (!NewNode)
		{
			return ErrorJson(TEXT("AddNodeInstance returned null."));
		}
		NewSettings->Rename(nullptr, NewNode, REN_DontCreateRedirectors | REN_DoNotDirty);

		double PosX = 0.0, PosY = 0.0;
		if (Args->TryGetNumberField(TEXT("posX"), PosX)) { NewNode->PositionX = static_cast<int32>(PosX); }
		if (Args->TryGetNumberField(TEXT("posY"), PosY)) { NewNode->PositionY = static_cast<int32>(PosY); }

		// Optional initial settings.
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		TArray<FString> Applied, Failed;
		if (Args->TryGetObjectField(TEXT("settings"), PropsPtr) && PropsPtr)
		{
			ApplyProperties(NewSettings, *PropsPtr, Applied, Failed);
		}

		NewSettings->PostEditChange();
		NewNode->PostEditChange();
		Graph->PostEditChange();
		SaveLoadedAsset(Graph);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetStringField(TEXT("node"), NewNode->GetName());
		Result->SetStringField(TEXT("nodeType"), SettingsClass->GetName());
		Result->SetStringField(TEXT("title"), NewNode->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		return SuccessJson(Result);
	}

	FString ConnectPCGNodes(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SourceName, TargetName, SourcePin, TargetPin;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("sourceNode"), SourceName);
		Args->TryGetStringField(TEXT("targetNode"), TargetName);
		Args->TryGetStringField(TEXT("sourcePin"), SourcePin);
		Args->TryGetStringField(TEXT("targetPin"), TargetPin);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Src = FindNode(Graph, SourceName);
		UPCGNode* Dst = FindNode(Graph, TargetName);
		if (!Src || !Dst)
		{
			return ErrorJson(TEXT("Source or target node not found."));
		}
		FName SrcPinName = SourcePin.IsEmpty() && Src->GetOutputPins().Num() > 0
			? Src->GetOutputPins()[0]->Properties.Label : FName(*SourcePin);
		FName DstPinName = TargetPin.IsEmpty() && Dst->GetInputPins().Num() > 0
			? Dst->GetInputPins()[0]->Properties.Label : FName(*TargetPin);

		FScopedTransaction Transaction(LOCTEXT("ConnectPCGNodes", "Connect PCG Nodes"));
		Graph->Modify();
		Src->Modify();
		Dst->Modify();
		Graph->AddEdge(Src, SrcPinName, Dst, DstPinName);

		// Verify the edge actually persisted (known UE PCG quirk where AddEdge can no-op).
		bool bVerified = false;
		for (const TObjectPtr<UPCGPin>& OutPin : Src->GetOutputPins())
		{
			if (OutPin && OutPin->Properties.Label == SrcPinName)
			{
				for (const TObjectPtr<UPCGEdge>& Edge : OutPin->Edges)
				{
					UPCGPin* Other = Edge ? ((Edge->InputPin == OutPin) ? Edge->OutputPin.Get() : Edge->InputPin.Get()) : nullptr;
					if (Other && Other->Node.Get() == Dst) { bVerified = true; break; }
				}
			}
		}
		Graph->PostEditChange();
		SaveLoadedAsset(Graph);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetStringField(TEXT("sourceNode"), Src->GetName());
		Result->SetStringField(TEXT("targetNode"), Dst->GetName());
		Result->SetStringField(TEXT("sourcePin"), SrcPinName.ToString());
		Result->SetStringField(TEXT("targetPin"), DstPinName.ToString());
		Result->SetBoolField(TEXT("edgeVerified"), bVerified);
		return SuccessJson(Result);
	}

	FString DisconnectPCGNodes(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SourceName, TargetName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("sourceNode"), SourceName);
		Args->TryGetStringField(TEXT("targetNode"), TargetName);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Src = FindNode(Graph, SourceName);
		UPCGNode* Dst = FindNode(Graph, TargetName);
		if (!Src || !Dst)
		{
			return ErrorJson(TEXT("Source or target node not found."));
		}
		FScopedTransaction Transaction(LOCTEXT("DisconnectPCGNodes", "Disconnect PCG Nodes"));
		Graph->Modify();
		Src->Modify();
		Dst->Modify();
		int32 Removed = 0;
		for (const TObjectPtr<UPCGPin>& OutPin : Src->GetOutputPins())
		{
			if (!OutPin) { continue; }
			TArray<TObjectPtr<UPCGEdge>> EdgesCopy = OutPin->Edges;
			for (const TObjectPtr<UPCGEdge>& Edge : EdgesCopy)
			{
				if (!Edge) { continue; }
				UPCGPin* Other = (Edge->InputPin == OutPin) ? Edge->OutputPin.Get() : Edge->InputPin.Get();
				if (Other && Other->Node.Get() == Dst)
				{
					Other->Edges.Remove(Edge);
					OutPin->Edges.Remove(Edge);
					++Removed;
				}
			}
		}
		Graph->PostEditChange();
		SaveLoadedAsset(Graph);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetNumberField(TEXT("removedEdges"), Removed);
		return SuccessJson(Result);
	}

	FString RemovePCGNode(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NodeName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("node"), NodeName);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Node = FindNode(Graph, NodeName);
		if (!Node)
		{
			return ErrorJson(FString::Printf(TEXT("Node '%s' not found."), *NodeName));
		}
		FScopedTransaction Transaction(LOCTEXT("RemovePCGNode", "Remove PCG Node"));
		Graph->Modify();
		Graph->RemoveNode(Node);
		Graph->PostEditChange();
		SaveLoadedAsset(Graph);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetBoolField(TEXT("removed"), true);
		return SuccessJson(Result);
	}

	FString SetPCGNodeSettings(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NodeName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("node"), NodeName);
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (!Args->TryGetObjectField(TEXT("settings"), PropsPtr) || !PropsPtr)
		{
			return ErrorJson(TEXT("Missing 'settings' object."));
		}
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Node = FindNode(Graph, NodeName);
		if (!Node)
		{
			return ErrorJson(FString::Printf(TEXT("Node '%s' not found."), *NodeName));
		}
		UPCGSettings* Settings = const_cast<UPCGSettings*>(Node->GetSettings());
		if (!Settings)
		{
			return ErrorJson(TEXT("Node has no settings."));
		}
		FScopedTransaction Transaction(LOCTEXT("SetPCGNodeSettings", "Set PCG Node Settings"));
		Settings->Modify();
		TArray<FString> Applied, Failed;
		ApplyProperties(Settings, *PropsPtr, Applied, Failed);
		Settings->PostEditChange();
		Graph->PostEditChange();
		SaveLoadedAsset(Graph);

		TArray<TSharedPtr<FJsonValue>> AppliedJson, FailedJson;
		for (const FString& A : Applied) { AppliedJson.Add(MakeShared<FJsonValueString>(A)); }
		for (const FString& F : Failed) { FailedJson.Add(MakeShared<FJsonValueString>(F)); }
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetStringField(TEXT("node"), Node->GetName());
		Result->SetArrayField(TEXT("applied"), AppliedJson);
		Result->SetArrayField(TEXT("failed"), FailedJson);
		return SuccessJson(Result);
	}

	FString ReadPCGNodeSettings(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NodeName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("node"), NodeName);
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Node = FindNode(Graph, NodeName);
		if (!Node || !Node->GetSettings())
		{
			return ErrorJson(FString::Printf(TEXT("Node '%s' not found."), *NodeName));
		}
		const UPCGSettings* Settings = Node->GetSettings();
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
		{
			FProperty* P = *It;
			if (!P->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			FString Out;
			P->ExportTextItem_Direct(Out, P->ContainerPtrToValuePtr<void>(Settings), nullptr, nullptr, PPF_None);
			Props->SetStringField(P->GetName(), Out);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("node"), Node->GetName());
		Result->SetStringField(TEXT("settingsClass"), Settings->GetClass()->GetName());
		Result->SetObjectField(TEXT("settings"), Props);

		// Object-pointer sub-settings export only as a reference path via ExportTextItem, so the
		// spawner's actual mesh list stays invisible in 'settings'. Surface it explicitly.
		if (const UPCGStaticMeshSpawnerSettings* Spawner = Cast<UPCGStaticMeshSpawnerSettings>(Settings))
		{
			if (const UPCGMeshSelectorWeighted* Sel = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters))
			{
				TArray<TSharedPtr<FJsonValue>> Entries;
				for (const FPCGMeshSelectorWeightedEntry& E : Sel->MeshEntries)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("mesh"), E.Descriptor.StaticMesh.ToSoftObjectPath().ToString());
					Obj->SetNumberField(TEXT("weight"), E.Weight);
					Entries.Add(MakeShared<FJsonValueObject>(Obj));
				}
				Result->SetArrayField(TEXT("meshEntries"), Entries);
			}
		}
		return SuccessJson(Result);
	}

	FString SetStaticMeshSpawnerMeshes(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NodeName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("node"), NodeName);
		const TArray<TSharedPtr<FJsonValue>>* MeshArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("meshes"), MeshArr) || !MeshArr)
		{
			return ErrorJson(TEXT("Missing 'meshes' array of StaticMesh asset paths."));
		}
		UPCGGraph* Graph = LoadGraph(AssetPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *AssetPath));
		}
		UPCGNode* Node = FindNode(Graph, NodeName);
		UPCGStaticMeshSpawnerSettings* Spawner = Node ? Cast<UPCGStaticMeshSpawnerSettings>(const_cast<UPCGSettings*>(Node->GetSettings())) : nullptr;
		if (!Spawner)
		{
			return ErrorJson(FString::Printf(TEXT("Node '%s' is not a StaticMeshSpawner."), *NodeName));
		}
		UPCGMeshSelectorWeighted* Selector = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters);
		if (!Selector)
		{
			Spawner->SetMeshSelectorType(UPCGMeshSelectorWeighted::StaticClass());
			Selector = Cast<UPCGMeshSelectorWeighted>(Spawner->MeshSelectorParameters);
		}
		if (!Selector)
		{
			return ErrorJson(TEXT("Could not set up a weighted mesh selector."));
		}
		FScopedTransaction Transaction(LOCTEXT("SetSMSMeshes", "Set PCG Spawner Meshes"));
		Selector->Modify();
		Spawner->Modify();
		Selector->MeshEntries.Reset();
		int32 Added = 0;
		TArray<FString> Resolved;
		TArray<FString> Failed;
		for (const TSharedPtr<FJsonValue>& V : *MeshArr)
		{
			const FString MeshPath = V->AsString();
			if (MeshPath.IsEmpty()) { continue; }

			// Resolve to a real asset so we never store a silent "mesh = None" entry. Accept
			// both full object paths (/Game/.../SM_X.SM_X) and package-only paths (/Game/.../SM_X);
			// for the latter, retry with the trailing asset name appended (.SM_X).
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
			if (!Mesh && !MeshPath.Contains(TEXT(".")))
			{
				int32 SlashIndex = INDEX_NONE;
				if (MeshPath.FindLastChar(TCHAR('/'), SlashIndex))
				{
					const FString AssetName = MeshPath.RightChop(SlashIndex + 1);
					Mesh = LoadObject<UStaticMesh>(nullptr, *(MeshPath + TEXT(".") + AssetName));
				}
			}
			if (!Mesh)
			{
				Failed.Add(MeshPath);
				continue;
			}

			FPCGMeshSelectorWeightedEntry Entry;
			Entry.Descriptor.StaticMesh = Mesh;
			Entry.Weight = 1;
			Selector->MeshEntries.Add(Entry);
			Resolved.Add(Mesh->GetPathName());
			++Added;
		}

		// A spawner with zero resolvable meshes silently produces zero instances downstream,
		// which previously read as "success" (entriesAdded:1 with a None mesh). Fail loudly.
		if (Added == 0)
		{
			return ErrorJson(FString::Printf(
				TEXT("No mesh paths resolved to a StaticMesh asset; nothing was set. Unresolved: %s"),
				*FString::Join(Failed, TEXT(", "))));
		}
#if WITH_EDITOR
		Selector->RefreshDisplayNames();
#endif
		Spawner->PostEditChange();
		SaveLoadedAsset(Graph);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Graph->GetPathName());
		Result->SetStringField(TEXT("node"), Node->GetName());
		Result->SetNumberField(TEXT("entriesAdded"), Added);
		TArray<TSharedPtr<FJsonValue>> ResolvedJson;
		for (const FString& P : Resolved) { ResolvedJson.Add(MakeShared<FJsonValueString>(P)); }
		Result->SetArrayField(TEXT("meshes"), ResolvedJson);
		if (Failed.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> FailedJson;
			for (const FString& P : Failed) { FailedJson.Add(MakeShared<FJsonValueString>(P)); }
			Result->SetArrayField(TEXT("failed"), FailedJson);
		}
		return SuccessJson(Result);
	}

	FString AddPCGVolume(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}
		FString GraphPath;
		Args->TryGetStringField(TEXT("graphPath"), GraphPath);
		UPCGGraph* Graph = GraphPath.IsEmpty() ? nullptr : LoadGraph(GraphPath);
		if (!GraphPath.IsEmpty() && !Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
		}

		FVector Location(0, 0, 0);
		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		if (Args->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
		{
			double X = 0, Y = 0, Z = 0;
			(*LocObj)->TryGetNumberField(TEXT("x"), X);
			(*LocObj)->TryGetNumberField(TEXT("y"), Y);
			(*LocObj)->TryGetNumberField(TEXT("z"), Z);
			Location = FVector(X, Y, Z);
		}

		FTransform SpawnTransform(Location);
		APCGVolume* Volume = World->SpawnActor<APCGVolume>(APCGVolume::StaticClass(), SpawnTransform);
		if (!Volume)
		{
			return ErrorJson(TEXT("Failed to spawn PCG volume."));
		}
		// Scale the volume's brush via the actor scale (extent in metres, default 25m cube-ish).
		double Scale = 25.0;
		Args->TryGetNumberField(TEXT("scale"), Scale);
		Volume->SetActorScale3D(FVector(Scale));

		FString Label;
		if (Args->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Volume->SetActorLabel(Label);
		}
		bool bGenerated = false;
		if (Graph)
		{
			if (UPCGComponent* Comp = Volume->FindComponentByClass<UPCGComponent>())
			{
				Comp->SetGraph(Graph);
				Comp->Generate(/*bForce*/true);
				bGenerated = true;
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), Volume->GetActorLabel());
		Result->SetStringField(TEXT("graph"), Graph ? Graph->GetPathName() : FString());
		Result->SetBoolField(TEXT("generated"), bGenerated);
		return SuccessJson(Result);
	}

	FString RegeneratePCG(const TSharedPtr<FJsonObject>& Args)
	{
		FString ActorLabel;
		Args->TryGetStringField(TEXT("actor"), ActorLabel);
		AActor* Actor = nullptr;
		UPCGComponent* Comp = FindPCGComponent(GetEditorWorld(), ActorLabel, Actor);
		if (!Comp)
		{
			return ErrorJson(FString::Printf(TEXT("No PCG component found on actor '%s'."), *ActorLabel));
		}
		double Seed = 0.0;
		if (Args->TryGetNumberField(TEXT("seed"), Seed))
		{
			Comp->Seed = static_cast<int32>(Seed);
		}
		Comp->Cleanup(/*bRemoveComponents*/true);
		Comp->Generate(/*bForce*/true);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : ActorLabel);
		Result->SetBoolField(TEXT("regenerated"), true);
		return SuccessJson(Result);
	}

	// Read back the per-instance transforms of the ISM / HISM components on an actor (and its
	// attached child actors) — typically what a PCG StaticMeshSpawner generated. PCG output is
	// ISM instances, not actors, so actor-level perception (analyze_spatial_relations) cannot see
	// them; this is the read side that lets autotune (and AI) measure spacing/overlap/count of a
	// generated PCG layout. Covers ISMs the actor owns; PCG partition actors are separate actors.
	FString EnumeratePCGInstances(const TSharedPtr<FJsonObject>& Args)
	{
		FString ActorLabel;
		Args->TryGetStringField(TEXT("actor"), ActorLabel);
		ActorLabel.TrimStartAndEndInline();
		if (ActorLabel.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'actor' (the level actor holding PCG-generated instances)."));
		}

		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		AActor* Target = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!IsValid(A)) { continue; }
			if (A->GetActorLabel() == ActorLabel || A->GetName() == ActorLabel)
			{
				Target = A;
				break;
			}
		}
		if (!Target)
		{
			return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel));
		}

		int32 MaxInstances = 5000;
		{
			double N = 0.0;
			if (Args->TryGetNumberField(TEXT("maxInstances"), N) && N >= 1.0)
			{
				MaxInstances = FMath::Min(static_cast<int32>(N), 200000);
			}
		}
		bool bSummaryOnly = false;
		Args->TryGetBoolField(TEXT("summaryOnly"), bSummaryOnly);

		// Gather ISM/HISM components on the actor plus its (recursively) attached child actors.
		TArray<UInstancedStaticMeshComponent*> Comps;
		Target->GetComponents<UInstancedStaticMeshComponent>(Comps, /*bIncludeFromChildActors*/ true);
		{
			TArray<AActor*> Attached;
			Target->GetAttachedActors(Attached, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
			for (AActor* Child : Attached)
			{
				if (!IsValid(Child)) { continue; }
				TArray<UInstancedStaticMeshComponent*> ChildComps;
				Child->GetComponents<UInstancedStaticMeshComponent>(ChildComps, /*bIncludeFromChildActors*/ true);
				Comps.Append(ChildComps);
			}
		}

		int32 TotalInstances = 0;
		bool bTruncated = false;
		TArray<TSharedPtr<FJsonValue>> CompArr, InstArr;
		for (UInstancedStaticMeshComponent* C : Comps)
		{
			if (!IsValid(C)) { continue; }
			const int32 Count = C->GetInstanceCount();
			TotalInstances += Count;

			UStaticMesh* Mesh = C->GetStaticMesh();
			const FString MeshPath = Mesh ? Mesh->GetPathName() : FString(TEXT("None"));

			TSharedRef<FJsonObject> CE = MakeShared<FJsonObject>();
			CE->SetStringField(TEXT("component"), C->GetName());
			CE->SetStringField(TEXT("owner"), C->GetOwner() ? C->GetOwner()->GetActorLabel() : FString());
			CE->SetStringField(TEXT("mesh"), MeshPath);
			CE->SetNumberField(TEXT("instanceCount"), Count);
			CE->SetBoolField(TEXT("isHISM"), C->IsA<UHierarchicalInstancedStaticMeshComponent>());
			CompArr.Add(MakeShared<FJsonValueObject>(CE));

			if (bSummaryOnly) { continue; }

			for (int32 i = 0; i < Count; ++i)
			{
				if (InstArr.Num() >= MaxInstances) { bTruncated = true; break; }
				FTransform T;
				if (!C->GetInstanceTransform(i, T, /*bWorldSpace*/ true)) { continue; }
				const FVector L = T.GetLocation();
				const FRotator R = T.Rotator();
				const FVector S = T.GetScale3D();

				TArray<TSharedPtr<FJsonValue>> LocArr;
				LocArr.Add(MakeShared<FJsonValueNumber>(L.X));
				LocArr.Add(MakeShared<FJsonValueNumber>(L.Y));
				LocArr.Add(MakeShared<FJsonValueNumber>(L.Z));
				TArray<TSharedPtr<FJsonValue>> ScaleArr;
				ScaleArr.Add(MakeShared<FJsonValueNumber>(S.X));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(S.Y));
				ScaleArr.Add(MakeShared<FJsonValueNumber>(S.Z));
				TSharedRef<FJsonObject> Rot = MakeShared<FJsonObject>();
				Rot->SetNumberField(TEXT("pitch"), R.Pitch);
				Rot->SetNumberField(TEXT("yaw"), R.Yaw);
				Rot->SetNumberField(TEXT("roll"), R.Roll);

				TSharedRef<FJsonObject> IE = MakeShared<FJsonObject>();
				IE->SetStringField(TEXT("component"), C->GetName());
				IE->SetStringField(TEXT("mesh"), MeshPath);
				IE->SetArrayField(TEXT("location"), LocArr);
				IE->SetObjectField(TEXT("rotation"), Rot);
				IE->SetArrayField(TEXT("scale"), ScaleArr);
				InstArr.Add(MakeShared<FJsonValueObject>(IE));
			}
			if (bTruncated) { break; }
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), Target->GetActorLabel());
		Result->SetNumberField(TEXT("componentCount"), CompArr.Num());
		Result->SetNumberField(TEXT("instanceCount"), TotalInstances);
		Result->SetArrayField(TEXT("components"), CompArr);
		if (!bSummaryOnly)
		{
			Result->SetArrayField(TEXT("instances"), InstArr);
			Result->SetNumberField(TEXT("instancesReturned"), InstArr.Num());
			Result->SetBoolField(TEXT("truncated"), bTruncated);
		}
		return SuccessJson(Result);
	}

	// Bind a PCG graph to an existing PCGComponent on a level actor. Uses UPCGComponent::SetGraph,
	// which correctly writes the GraphInstance — the reflection path (set_object_property) only hits
	// the deprecated transient Graph field and leaves GraphInstance.graph = None, so the component
	// generates an empty graph. This replaces the execute_python comp.set_graph() fallback.
	FString SetPCGComponentGraph(const TSharedPtr<FJsonObject>& Args)
	{
		FString ActorLabel, GraphPath;
		Args->TryGetStringField(TEXT("actor"), ActorLabel);
		Args->TryGetStringField(TEXT("graphPath"), GraphPath);
		if (GraphPath.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'graphPath' (the PCG graph asset to bind)."));
		}
		UPCGGraph* Graph = LoadGraph(GraphPath);
		if (!Graph)
		{
			return ErrorJson(FString::Printf(TEXT("PCG graph '%s' not found."), *GraphPath));
		}
		AActor* Actor = nullptr;
		UPCGComponent* Comp = FindPCGComponent(GetEditorWorld(), ActorLabel, Actor);
		if (!Comp)
		{
			return ErrorJson(FString::Printf(
				TEXT("No PCGComponent found on actor '%s'. Add one first: add_component class=/Script/PCG.PCGComponent."),
				*ActorLabel));
		}

		FScopedTransaction Transaction(LOCTEXT("SetPCGComponentGraph", "Set PCG Component Graph"));
		Comp->Modify();
		Comp->SetGraph(Graph);

		bool bRegenerate = true;
		Args->TryGetBoolField(TEXT("regenerate"), bRegenerate);
		bool bRegenerated = false;
		if (bRegenerate)
		{
			Comp->Cleanup(/*bRemoveComponents*/ true);
			Comp->Generate(/*bForce*/ true);
			bRegenerated = true;
		}

		// Verify against the effective graph (resolved through GraphInstance), not the transient field.
		const UPCGGraph* Bound = Comp->GetGraph();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : ActorLabel);
		Result->SetStringField(TEXT("graph"), Graph->GetPathName());
		Result->SetBoolField(TEXT("bound"), Bound == Graph);
		Result->SetStringField(TEXT("boundGraph"), Bound ? Bound->GetPathName() : FString(TEXT("None")));
		Result->SetBoolField(TEXT("regenerated"), bRegenerated);
		return SuccessJson(Result);
	}

	UStaticMesh* LoadStaticMeshAsset(const FString& Path)
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
		return LoadObject<UStaticMesh>(nullptr, *Normalized);
	}

	// Placement-oriented analysis of a StaticMesh: the dimensional + connection facts a scatter/road/
	// alignment rule needs to place the asset correctly (footprint for packing, pivot/ground offset for
	// alignment, sockets for modular snapping). Read-only; complements inspect_static_mesh (which covers
	// per-LOD tris / materials / collision counts). Shape/axis are heuristics, not authored truth.
	FString AnalyzeStaticMesh(const TSharedPtr<FJsonObject>& Args)
	{
		FString MeshPath;
		Args->TryGetStringField(TEXT("meshPath"), MeshPath);
		if (MeshPath.IsEmpty())
		{
			Args->TryGetStringField(TEXT("assetPath"), MeshPath); // accept either key
		}
		UStaticMesh* Mesh = LoadStaticMeshAsset(MeshPath);
		if (!Mesh)
		{
			return ErrorJson(FString::Printf(TEXT("StaticMesh '%s' not found."), *MeshPath));
		}

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const FVector Center = Bounds.Origin;     // local-space geometric centre
		const FVector Extent = Bounds.BoxExtent;  // half size
		const FVector Size = Extent * 2.0;
		const FVector Min = Center - Extent;
		const FVector Max = Center + Extent;
		const double ToM = 0.01;

		auto Vec = [](const FVector& V) -> TSharedRef<FJsonObject>
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("x"), V.X);
			O->SetNumberField(TEXT("y"), V.Y);
			O->SetNumberField(TEXT("z"), V.Z);
			return O;
		};

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
		Result->SetStringField(TEXT("units"), TEXT("raw fields in cm (UE units); *_m fields in metres"));
		Result->SetBoolField(TEXT("nanite"), Mesh->IsNaniteEnabled());
		Result->SetNumberField(TEXT("materialSlots"), Mesh->GetStaticMaterials().Num());

		// --- dimensions / footprint ---
		TSharedRef<FJsonObject> Dim = MakeShared<FJsonObject>();
		Dim->SetObjectField(TEXT("sizeCm"), Vec(Size));
		Dim->SetObjectField(TEXT("minCm"), Vec(Min));
		Dim->SetObjectField(TEXT("maxCm"), Vec(Max));
		Dim->SetObjectField(TEXT("centerCm"), Vec(Center));
		Dim->SetNumberField(TEXT("footprintX_m"), Size.X * ToM);
		Dim->SetNumberField(TEXT("footprintY_m"), Size.Y * ToM);
		Dim->SetNumberField(TEXT("height_m"), Size.Z * ToM);
		Result->SetObjectField(TEXT("dimensions"), Dim);

		// --- pivot understanding (pivot is local origin 0,0,0) ---
		TSharedRef<FJsonObject> Pivot = MakeShared<FJsonObject>();
		Pivot->SetObjectField(TEXT("pivotToCenterCm"), Vec(Center)); // offset from pivot to geometric centre
		const double GroundSnapOffsetZ = -Min.Z; // add to placement Z so the mesh base rests on the target plane
		Pivot->SetNumberField(TEXT("groundSnapOffsetZ_cm"), GroundSnapOffsetZ);
		const double EpsZ = FMath::Max(1.0, Size.Z * 0.05);
		FString VerticalPlacement = TEXT("custom");
		if (FMath::Abs(Min.Z) <= EpsZ) { VerticalPlacement = TEXT("base"); }
		else if (FMath::Abs(Center.Z) <= EpsZ) { VerticalPlacement = TEXT("center"); }
		else if (FMath::Abs(Max.Z) <= EpsZ) { VerticalPlacement = TEXT("top"); }
		const double EpsXY = FMath::Max(1.0, FMath::Max(Size.X, Size.Y) * 0.05);
		Pivot->SetStringField(TEXT("verticalPlacement"), VerticalPlacement);
		Pivot->SetBoolField(TEXT("centeredHorizontally"), FMath::Abs(Center.X) <= EpsXY && FMath::Abs(Center.Y) <= EpsXY);
		Result->SetObjectField(TEXT("pivot"), Pivot);

		// --- placement hints for scatter / packing / alignment rules ---
		TSharedRef<FJsonObject> Hints = MakeShared<FJsonObject>();
		Hints->SetNumberField(TEXT("packingRadius_m"), 0.5 * FMath::Max(Size.X, Size.Y) * ToM); // SelfPruning radius for no-overlap scatter
		Hints->SetNumberField(TEXT("groundSnapOffsetZ_cm"), GroundSnapOffsetZ);
		const double MaxH = FMath::Max(Size.X, Size.Y);
		const double MinH = FMath::Max(1.0, FMath::Min(Size.X, Size.Y));
		FString Shape = TEXT("blocky");
		if (Size.Z > 2.0 * MaxH) { Shape = TEXT("tall"); }       // pole / lamp / tree-ish
		else if (Size.Z < 0.25 * MinH) { Shape = TEXT("flat"); } // decal / ground / road-ish
		else if (MaxH > 3.0 * MinH) { Shape = TEXT("long"); }    // fence / wall / beam-ish
		Hints->SetStringField(TEXT("shape"), Shape);
		Hints->SetStringField(TEXT("longerHorizontalAxis"), Size.X >= Size.Y ? TEXT("X") : TEXT("Y"));
		Hints->SetStringField(TEXT("note"), TEXT("shape/axis are heuristics for category/forward inference, not authored truth"));
		Result->SetObjectField(TEXT("placementHints"), Hints);

		// --- sockets (connection points for modular / road snapping) ---
		TArray<TSharedPtr<FJsonValue>> Sockets;
		bool bHasConnector = false;
		for (const TObjectPtr<UStaticMeshSocket>& Socket : Mesh->Sockets)
		{
			if (!Socket)
			{
				continue;
			}
			const FString SocketName = Socket->SocketName.ToString();
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("name"), SocketName);
			S->SetObjectField(TEXT("locationCm"), Vec(Socket->RelativeLocation));
			TSharedRef<FJsonObject> Rot = MakeShared<FJsonObject>();
			Rot->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
			Rot->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
			Rot->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
			S->SetObjectField(TEXT("rotation"), Rot);
			const FString Lower = SocketName.ToLower();
			const bool bConnector =
				Lower.Contains(TEXT("entry")) || Lower.Contains(TEXT("exit")) ||
				Lower.Contains(TEXT("connect")) || Lower.Contains(TEXT("attach")) ||
				Lower.Contains(TEXT("socket")) || Lower.Contains(TEXT("snap"));
			S->SetBoolField(TEXT("looksLikeConnector"), bConnector);
			bHasConnector = bHasConnector || bConnector;
			Sockets.Add(MakeShared<FJsonValueObject>(S));
		}
		Result->SetArrayField(TEXT("sockets"), Sockets);
		Result->SetNumberField(TEXT("socketCount"), Sockets.Num());
		Result->SetBoolField(TEXT("hasConnectorSockets"), bHasConnector);

		return SuccessJson(Result);
	}

	// Measured extents of a mesh along a travel axis (X or Y), from LOD0 vertices.
	struct FTravelSpan
	{
		double Full = 0.0;        // full geometric extent (== bounding box along the axis)
		double TopSurface = 0.0;  // extent of the top-of-mesh band (walkable deck/road), <= Full
		bool bFromVertices = false;
	};

	// Measure how long a mesh is along its travel axis — the crux of seamless tiling, and exactly
	// what tripped the old workflow up. The bounding box is NOT the visible surface length: railings/
	// lips can overhang the deck so bbox(215) > deck(204), leaving an ~8cm see-through gap if you tile
	// at bbox pitch. We read LOD0 vertices directly (ISMs have no collision, so traces can't measure
	// this) and also compute the top-Z band's span — the actual walkable deck/road length — which is
	// the right default pitch for road/bridge tiling. Falls back to the bounding box if verts are
	// unavailable or the band looks pathological.
	static FTravelSpan MeasureTravelSpan(UStaticMesh* Mesh, int32 Axis)
	{
		FTravelSpan Span;
		const FBoxSphereBounds B = Mesh->GetBounds();
		Span.Full = 2.0 * ((Axis == 0) ? B.BoxExtent.X : B.BoxExtent.Y);
		Span.TopSurface = Span.Full;

		const FStaticMeshRenderData* RD = Mesh->GetRenderData();
		if (!RD || RD->LODResources.Num() == 0) { return Span; }
		const FPositionVertexBuffer& PVB = RD->LODResources[0].VertexBuffers.PositionVertexBuffer;
		const uint32 N = PVB.GetNumVertices();
		if (N == 0) { return Span; }

		double FwdMin = TNumericLimits<double>::Max(), FwdMax = TNumericLimits<double>::Lowest();
		double ZMin = TNumericLimits<double>::Max(), ZMax = TNumericLimits<double>::Lowest();
		for (uint32 i = 0; i < N; ++i)
		{
			const FVector3f P = PVB.VertexPosition(i);
			const double F = (Axis == 0) ? P.X : P.Y;
			FwdMin = FMath::Min(FwdMin, F); FwdMax = FMath::Max(FwdMax, F);
			ZMin = FMath::Min(ZMin, (double)P.Z); ZMax = FMath::Max(ZMax, (double)P.Z);
		}
		if (FwdMax > FwdMin) { Span.Full = FwdMax - FwdMin; }
		Span.bFromVertices = true;

		// Top band = vertices within the top 10% of height; their travel-axis span ≈ the deck/road length.
		const double Height = FMath::Max(1.0, ZMax - ZMin);
		const double BandZ = ZMax - Height * 0.10;
		double TopMin = TNumericLimits<double>::Max(), TopMax = TNumericLimits<double>::Lowest();
		int32 Count = 0;
		for (uint32 i = 0; i < N; ++i)
		{
			const FVector3f P = PVB.VertexPosition(i);
			if ((double)P.Z >= BandZ)
			{
				const double F = (Axis == 0) ? P.X : P.Y;
				TopMin = FMath::Min(TopMin, F); TopMax = FMath::Max(TopMax, F);
				++Count;
			}
		}
		if (Count > 0 && TopMax > TopMin)
		{
			const double Top = TopMax - TopMin;
			// Trust the band only when it's a sane fraction of the full extent; a thin railing cap could
			// otherwise yield a tiny, wildly-wrong pitch. Outside [50%,100%] of Full → fall back to Full.
			Span.TopSurface = (Top >= Span.Full * 0.5 && Top <= Span.Full) ? Top : Span.Full;
		}
		return Span;
	}

	// Lay rigid StaticMesh instances edge-to-edge along a spline so adjacent pieces abut with no gap —
	// the serial "next piece's trailing face = previous piece's leading face" placement the user asked
	// for. Unlike PCG's parallel arc-length sampler (which samples each point independently and lets
	// rigid tiles fan apart on curves), this marches piece-by-piece along the tangent by the mesh's
	// real length, so it stays seamless through curves. Output is one InstancedStaticMesh component.
	FString LayMeshesAlongSpline(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World) { return ErrorJson(TEXT("Editor world is not available.")); }

		// Scene-generation gate: laying meshes along a spline is content placement.
		{
			FString GateReason;
			if (!::WorldDataMCP::HasActiveSceneBrief(World->GetMapName(), GateReason))
			{
				return ErrorJson(GateReason);
			}
		}

		FString SplineActorLabel, MeshPath, ForwardAxis = TEXT("X"), TargetLabel, CompName = TEXT("LayMeshes_ISM");
		FString StartMeshPath, EndMeshPath;
		Args->TryGetStringField(TEXT("splineActor"), SplineActorLabel);
		Args->TryGetStringField(TEXT("mesh"), MeshPath);
		Args->TryGetStringField(TEXT("startMesh"), StartMeshPath);
		Args->TryGetStringField(TEXT("endMesh"), EndMeshPath);
		Args->TryGetStringField(TEXT("forwardAxis"), ForwardAxis);
		Args->TryGetStringField(TEXT("targetActor"), TargetLabel);
		Args->TryGetStringField(TEXT("componentName"), CompName);
		if (SplineActorLabel.IsEmpty()) { return ErrorJson(TEXT("Missing 'splineActor' (the actor holding the spline).")); }
		if (MeshPath.IsEmpty()) { return ErrorJson(TEXT("Missing 'mesh' (the middle/repeating StaticMesh to tile).")); }
		const bool bHasCaps = !StartMeshPath.IsEmpty() || !EndMeshPath.IsEmpty();

		UStaticMesh* Mesh = LoadStaticMeshAsset(MeshPath);
		if (!Mesh) { return ErrorJson(FString::Printf(TEXT("StaticMesh '%s' not found."), *MeshPath)); }

		int32 Axis = 0;
		const FString AxisUp = ForwardAxis.ToUpper();
		if (AxisUp == TEXT("X")) { Axis = 0; }
		else if (AxisUp == TEXT("Y")) { Axis = 1; }
		else { return ErrorJson(TEXT("'forwardAxis' must be 'X' or 'Y' (the mesh's travel axis along the spline).")); }

		// Locate the spline actor + its spline component.
		AActor* SplineActor = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == SplineActorLabel || It->GetName() == SplineActorLabel) { SplineActor = *It; break; }
		}
		if (!SplineActor) { return ErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *SplineActorLabel)); }
		USplineComponent* Spline = SplineActor->FindComponentByClass<USplineComponent>();
		if (!Spline) { return ErrorJson(FString::Printf(TEXT("Actor '%s' has no SplineComponent."), *SplineActorLabel)); }

		const double SplineLen = Spline->GetSplineLength();
		if (SplineLen <= 1.0) { return ErrorJson(TEXT("Spline length is ~0.")); }

		// Piece length along the travel axis — the crux of seamless tiling. Measured from LOD0 vertices,
		// NOT just the bounding box (bbox can be longer than the visible deck/road surface, which is what
		// leaves a see-through gap). Default 'auto' uses the top-surface (deck) span; 'bounds' uses the
		// full bbox extent (correct for clean modular tiles whose ends span the whole box); 'pieceLength'
		// overrides outright. Erring toward the shorter deck span means slight overlap, never a gap.
		const FBoxSphereBounds B = Mesh->GetBounds();
		const double Center = (Axis == 0) ? B.Origin.X : B.Origin.Y; // pivot→geometric-center offset along travel axis
		const FTravelSpan Span = MeasureTravelSpan(Mesh, Axis);

		FString MeasureMode = TEXT("auto");
		Args->TryGetStringField(TEXT("measureMode"), MeasureMode);
		double Step = MeasureMode.Equals(TEXT("bounds"), ESearchCase::IgnoreCase) ? Span.Full : Span.TopSurface;

		double Override = 0.0;
		const bool bOverridden = Args->TryGetNumberField(TEXT("pieceLength"), Override) && Override > 1.0;
		if (bOverridden) { Step = Override; }
		if (Step <= 1.0) { return ErrorJson(TEXT("Computed piece length is ~0; pass 'pieceLength' explicitly.")); }

		int32 MaxPieces = 2000;
		double MaxNum = 0.0;
		if (Args->TryGetNumberField(TEXT("maxPieces"), MaxNum) && MaxNum > 0) { MaxPieces = FMath::Min(MaxPieces, static_cast<int32>(MaxNum)); }

		// Output actor + ISM component (reused by name so re-runs replace instances, not stack them).
		AActor* OutActor = SplineActor;
		if (!TargetLabel.IsEmpty())
		{
			OutActor = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == TargetLabel || It->GetName() == TargetLabel) { OutActor = *It; break; }
			}
			if (!OutActor)
			{
				OutActor = World->SpawnActor<AActor>(AActor::StaticClass(), Spline->GetComponentTransform());
				if (OutActor) { OutActor->SetActorLabel(TargetLabel); }
			}
		}
		if (!OutActor) { return ErrorJson(TEXT("Could not resolve an output actor.")); }

		FScopedTransaction Transaction(LOCTEXT("LayMeshesAlongSpline", "Lay Meshes Along Spline"));
		OutActor->Modify();

		// Get-or-create an ISM by name on the output actor (one StaticMesh per ISM).
		auto FindISM = [&](const FString& Name) -> UInstancedStaticMeshComponent*
		{
			for (UActorComponent* C : OutActor->GetComponents())
			{
				if (UInstancedStaticMeshComponent* I = Cast<UInstancedStaticMeshComponent>(C))
				{
					if (I->GetName() == Name) { return I; }
				}
			}
			return nullptr;
		};
		auto MakeISM = [&](const FString& Name, UStaticMesh* M) -> UInstancedStaticMeshComponent*
		{
			UInstancedStaticMeshComponent* I = FindISM(Name);
			if (!I)
			{
				I = NewObject<UInstancedStaticMeshComponent>(OutActor, *Name);
				if (USceneComponent* Root = OutActor->GetRootComponent()) { I->SetupAttachment(Root); }
				else { OutActor->SetRootComponent(I); }
				I->RegisterComponent();
				OutActor->AddInstanceComponent(I);
			}
			I->Modify();
			I->SetStaticMesh(M);
			I->ClearInstances();
			return I;
		};

		const FVector Up = FVector::UpVector;
		const FString StartName = CompName + TEXT("_Start");
		const FString EndName = CompName + TEXT("_End");
		// Empty any cap components left from a previous run so a changed cap config can't leave stale instances.
		for (const FString& Nm : { StartName, EndName })
		{
			if (UInstancedStaticMeshComponent* I = FindISM(Nm)) { I->Modify(); I->ClearInstances(); }
		}

		auto SpanOf = [&](UStaticMesh* M) -> double
		{
			const FTravelSpan S = MeasureTravelSpan(M, Axis);
			return MeasureMode.Equals(TEXT("bounds"), ESearchCase::IgnoreCase) ? S.Full : S.TopSurface;
		};
		auto CenterOffset = [&](UStaticMesh* M) -> double
		{
			return (Axis == 0) ? M->GetBounds().Origin.X : M->GetBounds().Origin.Y;
		};
		const TCHAR* SourceLabel = bOverridden ? TEXT("override") : (MeasureMode.Equals(TEXT("bounds"), ESearchCase::IgnoreCase) ? TEXT("bounds") : TEXT("topSurface"));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), OutActor->GetActorLabel());
		Result->SetStringField(TEXT("splineActor"), SplineActor->GetActorLabel());
		Result->SetNumberField(TEXT("splineLength"), SplineLen);
		Result->SetStringField(TEXT("pieceLengthSource"), SourceLabel);

		if (!bHasCaps)
		{
			// Single-mesh path: march piece centers edge-to-edge by chord (gapless even on curves).
			UInstancedStaticMeshComponent* ISM = MakeISM(CompName, Mesh);
			FVector Front = Spline->GetLocationAtDistanceAlongSpline(0.0, ESplineCoordinateSpace::World);
			double Arc = 0.0;
			int32 Placed = 0;
			while (Arc < SplineLen && Placed < MaxPieces)
			{
				FVector Tangent = Spline->GetDirectionAtDistanceAlongSpline(FMath::Min(Arc + Step * 0.5, SplineLen), ESplineCoordinateSpace::World).GetSafeNormal();
				if (Tangent.IsNearlyZero()) { Tangent = FVector::ForwardVector; }
				const FRotator Rot = (Axis == 0) ? FRotationMatrix::MakeFromXZ(Tangent, Up).Rotator() : FRotationMatrix::MakeFromYZ(Tangent, Up).Rotator();
				ISM->AddInstance(FTransform(Rot, Front + Tangent * (Step * 0.5 - Center), FVector(1.0)), /*bWorldSpace*/ true);
				Front += Tangent * Step;
				Arc += Step;
				++Placed;
			}
			ISM->MarkRenderStateDirty();
			Result->SetStringField(TEXT("component"), ISM->GetName());
			Result->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			Result->SetNumberField(TEXT("placed"), Placed);
			Result->SetNumberField(TEXT("pieceLength"), Step);
			Result->SetNumberField(TEXT("boundsSpan"), Span.Full);
			Result->SetNumberField(TEXT("topSurfaceSpan"), Span.TopSurface);
			Result->SetBoolField(TEXT("measuredFromVertices"), Span.bFromVertices);
			Result->SetBoolField(TEXT("reachedCap"), Placed >= MaxPieces);
			return SuccessJson(Result);
		}

		// Three-piece path: start cap → middle fill → end cap, each its own measured length, all chained
		// edge-to-edge. A distinct cap mesh gets its own ISM (one mesh per ISM); a cap equal to the middle reuses.
		UStaticMesh* StartM = StartMeshPath.IsEmpty() ? Mesh : LoadStaticMeshAsset(StartMeshPath);
		UStaticMesh* EndM = EndMeshPath.IsEmpty() ? Mesh : LoadStaticMeshAsset(EndMeshPath);
		if (!StartM) { return ErrorJson(FString::Printf(TEXT("startMesh '%s' not found."), *StartMeshPath)); }
		if (!EndM) { return ErrorJson(FString::Printf(TEXT("endMesh '%s' not found."), *EndMeshPath)); }

		UInstancedStaticMeshComponent* MidISM = MakeISM(CompName, Mesh);
		UInstancedStaticMeshComponent* StartISM = (StartM == Mesh) ? MidISM : MakeISM(StartName, StartM);
		UInstancedStaticMeshComponent* EndISM = (EndM == Mesh) ? MidISM : ((EndM == StartM) ? StartISM : MakeISM(EndName, EndM));

		const double StartLen = SpanOf(StartM);
		const double MidLen = bOverridden ? Override : SpanOf(Mesh);
		const double EndLen = SpanOf(EndM);
		if (StartLen <= 1.0 || MidLen <= 1.0 || EndLen <= 1.0) { return ErrorJson(TEXT("A measured piece length is ~0; pass 'pieceLength'.")); }

		// Same cap mesh at both ends → flip the end cap 180° so both caps open outward (the common modular
		// pattern). Override with 'flipEnd'.
		bool bFlipEnd = (EndM == StartM);
		Args->TryGetBoolField(TEXT("flipEnd"), bFlipEnd);

		FVector Front = Spline->GetLocationAtDistanceAlongSpline(0.0, ESplineCoordinateSpace::World);
		double Arc = 0.0;
		int32 Placed = 0, MidCount = 0;
		auto PlacePiece = [&](UInstancedStaticMeshComponent* ISM, UStaticMesh* M, double Len, bool bFlip)
		{
			FVector Tangent = Spline->GetDirectionAtDistanceAlongSpline(FMath::Clamp(Arc + Len * 0.5, 0.0, SplineLen), ESplineCoordinateSpace::World).GetSafeNormal();
			if (Tangent.IsNearlyZero()) { Tangent = FVector::ForwardVector; }
			FQuat Q = (Axis == 0) ? FRotationMatrix::MakeFromXZ(Tangent, Up).ToQuat() : FRotationMatrix::MakeFromYZ(Tangent, Up).ToQuat();
			if (bFlip) { Q = Q * FQuat(FVector::UpVector, PI); }
			ISM->AddInstance(FTransform(Q, Front + Tangent * (Len * 0.5 - CenterOffset(M)), FVector(1.0)), /*bWorldSpace*/ true);
			Front += Tangent * Len;
			Arc += Len;
			++Placed;
		};

		PlacePiece(StartISM, StartM, StartLen, false);
		while (Arc + MidLen + EndLen <= SplineLen && Placed < MaxPieces) { PlacePiece(MidISM, Mesh, MidLen, false); ++MidCount; }
		bool bEndPlaced = false;
		if (Arc + EndLen * 0.5 <= SplineLen && Placed < MaxPieces) { PlacePiece(EndISM, EndM, EndLen, bFlipEnd); bEndPlaced = true; }

		MidISM->MarkRenderStateDirty();
		if (StartISM != MidISM) { StartISM->MarkRenderStateDirty(); }
		if (EndISM != MidISM && EndISM != StartISM) { EndISM->MarkRenderStateDirty(); }

		Result->SetStringField(TEXT("component"), MidISM->GetName());
		Result->SetStringField(TEXT("startMesh"), StartM->GetPathName());
		Result->SetStringField(TEXT("middleMesh"), Mesh->GetPathName());
		Result->SetStringField(TEXT("endMesh"), EndM->GetPathName());
		Result->SetBoolField(TEXT("startPlaced"), true);
		Result->SetNumberField(TEXT("middlePlaced"), MidCount);
		Result->SetBoolField(TEXT("endPlaced"), bEndPlaced);
		Result->SetBoolField(TEXT("endFlipped"), bEndPlaced && bFlipEnd);
		Result->SetNumberField(TEXT("placed"), Placed);
		Result->SetNumberField(TEXT("startLength"), StartLen);
		Result->SetNumberField(TEXT("pieceLength"), MidLen);
		Result->SetNumberField(TEXT("endLength"), EndLen);
		Result->SetBoolField(TEXT("reachedCap"), Placed >= MaxPieces);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"list_pcg_graphs","description":"List PCGGraph assets in the project.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"List PCG Graphs","readOnlyHint":true,"openWorldHint":false}},
{"name":"create_pcg_graph","description":"Create an empty PCGGraph asset.","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/PCG/PCG_Forest."}},"required":["destPath"]},"annotations":{"title":"Create PCG Graph","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_pcg_graph","description":"Read a PCG graph's nodes (name/title/pins) and edges (from/fromPin/to/toPin).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Read PCG Graph","readOnlyHint":true,"openWorldHint":false}},
{"name":"add_pcg_node","description":"Add a node to a PCG graph by settings type (e.g. SurfaceSampler, TransformPoints, DensityFilter, StaticMeshSpawner, Difference, SplineSampler, GetActorData). Optional 'settings' object applies node properties, plus posX/posY.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"nodeType":{"type":"string"},"settings":{"type":"object"},"posX":{"type":"number"},"posY":{"type":"number"}},"required":["assetPath","nodeType"]},"annotations":{"title":"Add PCG Node","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"connect_pcg_nodes","description":"Connect a source node's output pin to a target node's input pin. Pins default to the first output/input when omitted. Returns edgeVerified.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"sourceNode":{"type":"string"},"targetNode":{"type":"string"},"sourcePin":{"type":"string"},"targetPin":{"type":"string"}},"required":["assetPath","sourceNode","targetNode"]},"annotations":{"title":"Connect PCG Nodes","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"disconnect_pcg_nodes","description":"Remove edges between two PCG nodes.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"sourceNode":{"type":"string"},"targetNode":{"type":"string"}},"required":["assetPath","sourceNode","targetNode"]},"annotations":{"title":"Disconnect PCG Nodes","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"remove_pcg_node","description":"Remove a node from a PCG graph by name.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"node":{"type":"string"}},"required":["assetPath","node"]},"annotations":{"title":"Remove PCG Node","readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
)JSON")
TEXT(R"JSON(
{"name":"set_pcg_node_settings","description":"Set properties on a PCG node's settings via reflection. 'settings' is a name->value map; keys may be dotted paths into nested structs/objects (e.g. 'SamplerParams.Mode', 'SamplerParams.DistanceIncrement'); object refs accept asset paths. Check the returned 'failed' array — a non-empty list means those keys silently did NOT apply.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"node":{"type":"string"},"settings":{"type":"object"}},"required":["assetPath","node","settings"]},"annotations":{"title":"Set PCG Node Settings","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"read_pcg_node_settings","description":"Read a PCG node's editable settings (property name -> exported text) and settings class. For a StaticMeshSpawner also returns 'meshEntries' (mesh path + weight) from the weighted selector sub-object, which the plain reflection dump cannot show.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"node":{"type":"string"}},"required":["assetPath","node"]},"annotations":{"title":"Read PCG Node Settings","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_static_mesh_spawner_meshes","description":"Set the StaticMesh entries on a StaticMeshSpawner node (weighted selector). 'meshes' is an array of StaticMesh asset paths.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"node":{"type":"string"},"meshes":{"type":"array","items":{"type":"string"}}},"required":["assetPath","node","meshes"]},"annotations":{"title":"Set Spawner Meshes","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_pcg_volume","description":"Spawn a PCG volume actor in the level, optionally assign a graph and generate it. 'scale' sizes the volume (default 25).","inputSchema":{"type":"object","properties":{"graphPath":{"type":"string"},"location":{"type":"object","description":"{x,y,z}"},"scale":{"type":"number"},"label":{"type":"string"}}},"annotations":{"title":"Add PCG Volume","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"regenerate_pcg","description":"Cleanup and regenerate a PCG component on a level actor (optionally set its seed first).","inputSchema":{"type":"object","properties":{"actor":{"type":"string"},"seed":{"type":"number"}},"required":["actor"]},"annotations":{"title":"Regenerate PCG","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"enumerate_pcg_instances","description":"Read back the per-instance transforms of the InstancedStaticMesh/HISM components on an actor (and its attached child actors) — i.e. what a PCG StaticMeshSpawner generated. PCG output is ISM instances (not actors), so actor-level tools like analyze_spatial_relations cannot see them; this is the read side that lets autotune and AI measure spacing/overlap/count of a generated PCG layout. Always returns a per-component summary; adds per-instance {mesh, location[x,y,z], rotation{pitch,yaw,roll}, scale[x,y,z]} unless summaryOnly. Covers ISMs the actor owns; PCG partition actors are separate actors and are not followed.","inputSchema":{"type":"object","properties":{"actor":{"type":"string","description":"Level actor holding the PCG-generated instances."},"maxInstances":{"type":"number","description":"Cap on per-instance rows. Default 5000, capped 200000; sets truncated=true when hit."},"summaryOnly":{"type":"boolean","description":"Return only the per-component summary, no per-instance rows. Default false."}},"required":["actor"]},"annotations":{"title":"Enumerate PCG Instances","readOnlyHint":true,"openWorldHint":false}},
{"name":"set_pcg_component_graph","description":"Bind a PCG graph asset to an existing PCGComponent on a level actor via SetGraph (correctly writes the GraphInstance). Use this to attach a graph to a spline/actor's PCGComponent — set_object_property cannot, it only hits the deprecated transient Graph field and leaves the component generating an empty graph. Regenerates by default; returns 'bound' (verified against the effective graph).","inputSchema":{"type":"object","properties":{"actor":{"type":"string"},"graphPath":{"type":"string"},"regenerate":{"type":"boolean","description":"Regenerate after binding (default true)."}},"required":["actor","graphPath"]},"annotations":{"title":"Set PCG Component Graph","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"analyze_static_mesh","description":"Analyze a StaticMesh for PLACEMENT: bounds/footprint(m), pivot vertical placement + groundSnapOffsetZ, packing radius (no-overlap scatter), shape/axis heuristics, and sockets (with transforms + connector detection for modular/road snapping). Placement-focused companion to inspect_static_mesh.","inputSchema":{"type":"object","properties":{"meshPath":{"type":"string","description":"StaticMesh asset path, e.g. /Game/.../SM_StreetLamp."},"assetPath":{"type":"string","description":"Alias for meshPath."}},"required":["meshPath"]},"annotations":{"title":"Analyze Static Mesh","readOnlyHint":true,"openWorldHint":false}},
{"name":"lay_meshes_along_spline","description":"Tile StaticMesh(es) edge-to-edge along an actor's spline with NO gaps: each piece's trailing face is placed on the previous piece's leading face (serial chord march along the tangent). Use this instead of a PCG StaticMeshSpawner when you need seamless rigid pieces — it stays gapless through curves where equidistant PCG tiles fan apart. Optional 'startMesh'/'endMesh' make a 3-piece modular run (start cap → repeated 'mesh' middle → end cap), each measured independently; same mesh at both ends auto-flips the end cap 180° ('flipEnd' overrides). Piece length is MEASURED FROM LOD0 VERTICES (not the bounding box, which can be longer than the visible deck and leave a see-through gap): measureMode 'auto' (default) uses the top-surface/deck span, 'bounds' the full bbox; 'pieceLength' overrides the MIDDLE length. Result reports boundsSpan/topSurfaceSpan/pieceLengthSource (and start/middle/end counts for caps). 'forwardAxis' (X/Y) is the mesh's travel axis. Output is InstancedStaticMesh component(s) on 'targetActor' (default = the spline actor), reused by 'componentName' so re-runs replace instances. Not PCG-driven, so regenerate_pcg will not wipe it.","inputSchema":{"type":"object","properties":{"splineActor":{"type":"string"},"mesh":{"type":"string","description":"Middle/repeating mesh."},"startMesh":{"type":"string","description":"Optional start-cap mesh."},"endMesh":{"type":"string","description":"Optional end-cap mesh."},"flipEnd":{"type":"boolean","description":"Rotate the end cap 180° (default: true when start==end mesh)."},"forwardAxis":{"type":"string","description":"X or Y (default X)"},"measureMode":{"type":"string","description":"auto (top-surface/deck span, default) or bounds (full bbox)."},"pieceLength":{"type":"number","description":"Override the measured MIDDLE travel-axis length."},"targetActor":{"type":"string","description":"Actor to receive the ISM; default = the spline actor."},"componentName":{"type":"string"},"maxPieces":{"type":"number"}},"required":["splineActor","mesh"]},"annotations":{"title":"Lay Meshes Along Spline","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("list_pcg_graphs")) { OutResult = ListPCGGraphs(Args); return true; }
	if (ToolName == TEXT("create_pcg_graph")) { OutResult = CreatePCGGraph(Args); return true; }
	if (ToolName == TEXT("read_pcg_graph")) { OutResult = ReadPCGGraph(Args); return true; }
	if (ToolName == TEXT("add_pcg_node")) { OutResult = AddPCGNode(Args); return true; }
	if (ToolName == TEXT("connect_pcg_nodes")) { OutResult = ConnectPCGNodes(Args); return true; }
	if (ToolName == TEXT("disconnect_pcg_nodes")) { OutResult = DisconnectPCGNodes(Args); return true; }
	if (ToolName == TEXT("remove_pcg_node")) { OutResult = RemovePCGNode(Args); return true; }
	if (ToolName == TEXT("set_pcg_node_settings")) { OutResult = SetPCGNodeSettings(Args); return true; }
	if (ToolName == TEXT("read_pcg_node_settings")) { OutResult = ReadPCGNodeSettings(Args); return true; }
	if (ToolName == TEXT("set_static_mesh_spawner_meshes")) { OutResult = SetStaticMeshSpawnerMeshes(Args); return true; }
	if (ToolName == TEXT("add_pcg_volume")) { OutResult = AddPCGVolume(Args); return true; }
	if (ToolName == TEXT("regenerate_pcg")) { OutResult = RegeneratePCG(Args); return true; }
	if (ToolName == TEXT("enumerate_pcg_instances")) { OutResult = EnumeratePCGInstances(Args); return true; }
	if (ToolName == TEXT("set_pcg_component_graph")) { OutResult = SetPCGComponentGraph(Args); return true; }
	if (ToolName == TEXT("analyze_static_mesh")) { OutResult = AnalyzeStaticMesh(Args); return true; }
	if (ToolName == TEXT("lay_meshes_along_spline")) { OutResult = LayMeshesAlongSpline(Args); return true; }
	return false;
}
}
}

#undef LOCTEXT_NAMESPACE
