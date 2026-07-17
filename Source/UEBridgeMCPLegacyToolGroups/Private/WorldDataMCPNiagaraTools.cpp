#include "WorldDataMCPNiagaraTools.h"

#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace WorldDataMCP
{
namespace NiagaraTools
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

	void SaveAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

	FVersionedNiagaraEmitterData* ResolveEmitter(UNiagaraSystem* System, const FString& Name, int32 Index,
		UNiagaraEmitter*& OutEmitter, FGuid& OutVersion, FString& OutHandleName)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		int32 TargetIdx = INDEX_NONE;
		if (!Name.IsEmpty())
		{
			for (int32 i = 0; i < Handles.Num(); ++i)
			{
				if (Handles[i].GetName().ToString() == Name) { TargetIdx = i; break; }
			}
		}
		else if (Index >= 0 && Index < Handles.Num())
		{
			TargetIdx = Index;
		}
		else if (Handles.Num() > 0)
		{
			TargetIdx = 0;
		}
		if (!Handles.IsValidIndex(TargetIdx))
		{
			return nullptr;
		}
		FVersionedNiagaraEmitter VE = Handles[TargetIdx].GetInstance();
		OutEmitter = VE.Emitter;
		OutVersion = VE.Version;
		OutHandleName = Handles[TargetIdx].GetName().ToString();
		return VE.GetEmitterData();
	}

	// ---- tools ---------------------------------------------------------------------------

	FString AddEmitterToSystem(const TSharedPtr<FJsonObject>& Args)
	{
		FString SystemPath, SourcePath;
		Args->TryGetStringField(TEXT("assetPath"), SystemPath);
		Args->TryGetStringField(TEXT("sourceEmitter"), SourcePath);
		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemPath));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Niagara system '%s' not found."), *SystemPath));
		}
		UNiagaraEmitter* Source = Cast<UNiagaraEmitter>(LoadAssetObject(SourcePath));
		if (!Source)
		{
			return ErrorJson(FString::Printf(TEXT("Source emitter '%s' not found."), *SourcePath));
		}
		// Idempotency: skip if an emitter from the same source is already present.
		for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
		{
			if (H.GetInstance().Emitter == Source)
			{
				return ErrorJson(TEXT("An emitter from this source is already in the system."));
			}
		}
		const FGuid Version = Source->GetExposedVersion().VersionGuid;
		// Canonical safe add (raw System->AddEmitterHandle crashes on empty systems).
		const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Source, Version);
		System->PostEditChange();
		SaveAsset(System);

		FString StoredName;
		for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
		{
			if (H.GetId() == HandleId) { StoredName = H.GetName().ToString(); break; }
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		Result->SetStringField(TEXT("emitter"), StoredName);
		Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
		return SuccessJson(Result);
	}

	FString AddEmitterRenderer(const TSharedPtr<FJsonObject>& Args)
	{
		FString SystemPath, EmitterName, RendererType;
		Args->TryGetStringField(TEXT("assetPath"), SystemPath);
		Args->TryGetStringField(TEXT("emitter"), EmitterName);
		Args->TryGetStringField(TEXT("rendererType"), RendererType);
		double IndexNum = -1.0;
		Args->TryGetNumberField(TEXT("emitterIndex"), IndexNum);
		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemPath));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Niagara system '%s' not found."), *SystemPath));
		}
		UNiagaraEmitter* Emitter = nullptr;
		FGuid Version;
		FString HandleName;
		FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, static_cast<int32>(IndexNum), Emitter, Version, HandleName);
		if (!Emitter || !Data)
		{
			return ErrorJson(TEXT("Emitter not found in system."));
		}
		UClass* RendererClass = nullptr;
		if (RendererType.Equals(TEXT("mesh"), ESearchCase::IgnoreCase)) { RendererClass = UNiagaraMeshRendererProperties::StaticClass(); }
		else if (RendererType.Equals(TEXT("ribbon"), ESearchCase::IgnoreCase)) { RendererClass = UNiagaraRibbonRendererProperties::StaticClass(); }
		else { RendererClass = UNiagaraSpriteRendererProperties::StaticClass(); }

		UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
		Emitter->Modify();
		Emitter->AddRenderer(NewRenderer, Version);
		Emitter->PostEditChange();
		SaveAsset(System);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		Result->SetStringField(TEXT("emitter"), HandleName);
		Result->SetStringField(TEXT("renderer"), RendererClass->GetName());
		return SuccessJson(Result);
	}

	FString ListNiagaraParameters(const TSharedPtr<FJsonObject>& Args)
	{
		FString SystemPath;
		Args->TryGetStringField(TEXT("assetPath"), SystemPath);
		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemPath));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Niagara system '%s' not found."), *SystemPath));
		}
		const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
		TArray<FNiagaraVariable> Vars;
		UserParams.GetParameters(Vars);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FNiagaraVariable& V : Vars)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), V.GetName().ToString());
			E->SetStringField(TEXT("type"), V.GetType().GetName());
			E->SetBoolField(TEXT("isDataInterface"), V.IsDataInterface());
			Out.Add(MakeShared<FJsonValueObject>(E));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		Result->SetNumberField(TEXT("count"), Out.Num());
		Result->SetArrayField(TEXT("parameters"), Out);
		return SuccessJson(Result);
	}

	FString ReadEmitterRenderers(const TSharedPtr<FJsonObject>& Args)
	{
		FString SystemPath, EmitterName;
		Args->TryGetStringField(TEXT("assetPath"), SystemPath);
		Args->TryGetStringField(TEXT("emitter"), EmitterName);
		double IndexNum = -1.0;
		Args->TryGetNumberField(TEXT("emitterIndex"), IndexNum);
		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemPath));
		if (!System)
		{
			return ErrorJson(FString::Printf(TEXT("Niagara system '%s' not found."), *SystemPath));
		}
		UNiagaraEmitter* Emitter = nullptr;
		FGuid Version;
		FString HandleName;
		FVersionedNiagaraEmitterData* Data = ResolveEmitter(System, EmitterName, static_cast<int32>(IndexNum), Emitter, Version, HandleName);
		if (!Data)
		{
			return ErrorJson(TEXT("Emitter not found in system."));
		}
		TArray<TSharedPtr<FJsonValue>> Renderers;
		int32 Idx = 0;
		for (UNiagaraRendererProperties* R : Data->GetRenderers())
		{
			if (R)
			{
				TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetNumberField(TEXT("index"), Idx);
				E->SetStringField(TEXT("class"), R->GetClass()->GetName());
				E->SetBoolField(TEXT("enabled"), R->GetIsEnabled());
				Renderers.Add(MakeShared<FJsonValueObject>(E));
			}
			++Idx;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), System->GetPathName());
		Result->SetStringField(TEXT("emitter"), HandleName);
		Result->SetArrayField(TEXT("renderers"), Renderers);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"add_emitter_to_system","description":"Add an emitter (from a source emitter asset) to a Niagara system. Uses the safe NiagaraEditor helper.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Niagara system asset path."},"sourceEmitter":{"type":"string","description":"Source UNiagaraEmitter asset path."}},"required":["assetPath","sourceEmitter"]},"annotations":{"title":"Add Emitter To System","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_emitter_renderer","description":"Add a renderer to an emitter in a Niagara system. rendererType: sprite (default), mesh, ribbon. Emitter selected by name or emitterIndex.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"emitter":{"type":"string"},"emitterIndex":{"type":"number"},"rendererType":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"Add Emitter Renderer","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"list_niagara_parameters","description":"List a Niagara system's user-exposed parameters (name, type, isDataInterface).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"}},"required":["assetPath"]},"annotations":{"title":"List Niagara Parameters","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_emitter_renderers","description":"List the renderers on an emitter in a Niagara system (class, enabled).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"emitter":{"type":"string"},"emitterIndex":{"type":"number"}},"required":["assetPath"]},"annotations":{"title":"Read Emitter Renderers","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("add_emitter_to_system")) { OutResult = AddEmitterToSystem(Args); return true; }
	if (ToolName == TEXT("add_emitter_renderer")) { OutResult = AddEmitterRenderer(Args); return true; }
	if (ToolName == TEXT("list_niagara_parameters")) { OutResult = ListNiagaraParameters(Args); return true; }
	if (ToolName == TEXT("read_emitter_renderers")) { OutResult = ReadEmitterRenderers(Args); return true; }
	return false;
}
}
}
