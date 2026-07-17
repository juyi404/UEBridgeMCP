#include "WorldDataAgentBackendFactory.h"

#include "Dom/JsonObject.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WorldDataAgentBackend.h"
#include "WorldDataCodexACPClient.h"
#include "WorldDataCodexAppServerBackend.h"
#include "UEBridgeMCPCoreModule.h"

namespace
{
	class FUnavailableAgentBackend final : public IWorldDataAgentBackend
	{
	public:
		FUnavailableAgentBackend(const FString& InId, const FString& InDisplayName, const FString& InReason)
			: Id(InId), DisplayName(InDisplayName), Reason(InReason)
		{
		}

		virtual FString GetBackendId() const override { return Id; }
		virtual FString GetDisplayName() const override { return DisplayName; }
		virtual FWorldDataAgentBackendCapabilities GetCapabilities() const override { return FWorldDataAgentBackendCapabilities(); }
		virtual void SendPrompt(const FString&) override { Fail(); }
		virtual void Stop() override {}
		virtual void SetPermissionMode(EWorldDataCodexPermissionMode) override {}
		virtual EWorldDataCodexPermissionMode GetPermissionMode() const override { return EWorldDataCodexPermissionMode::Default; }
		virtual void RespondToPermission(int32, const FString&) override {}
		virtual bool IsRunning() const override { return false; }
		virtual bool IsReady() const override { return false; }
		virtual bool IsProcessing() const override { return false; }
		virtual FString GetLastError() const override { return Reason; }

	private:
		void Fail()
		{
			if (OnError.IsBound()) OnError.Execute(Reason);
		}

		FString Id;
		FString DisplayName;
		FString Reason;
	};

	static FString ReadSelection()
	{
		FString Selection = TEXT("auto");
		if (GConfig)
		{
			GConfig->GetString(TEXT("UEBridgeMCP.Agent"), TEXT("Backend"), Selection, GGameIni);
		}
		Selection.TrimStartAndEndInline();
		Selection.ToLowerInline();
		return Selection.IsEmpty() ? TEXT("auto") : Selection;
	}

	static FString NormalizeSelection(const FString& Selection)
	{
		if (Selection == TEXT("acp"))
		{
			return TEXT("acp_codex");
		}
		if (Selection == TEXT("cursor"))
		{
			return TEXT("acp_cursor");
		}
		if (Selection == TEXT("claude"))
		{
			return TEXT("acp_claude");
		}
		return Selection;
	}

	static bool IsKnownSelection(const FString& Selection)
	{
		return Selection == TEXT("auto")
			|| Selection == TEXT("codex_app_server")
			|| Selection == TEXT("acp_codex")
			|| Selection == TEXT("acp_cursor")
			|| Selection == TEXT("acp_claude");
	}

	static FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	static void ConfigureMcpConnection(const TSharedPtr<IWorldDataAgentBackend>& Backend)
	{
		if (!Backend.IsValid() || !Backend->GetBackendId().StartsWith(TEXT("acp_")))
		{
			return;
		}
		IWorldDataMCPService& Mcp = IUEBridgeMCPCoreModule::Get().GetService();
		if (!Mcp.IsRunning())
		{
			Mcp.StartConfigured();
		}
		FWorldDataAgentMcpConnection Connection;
		Connection.bAvailable = Mcp.IsRunning();
		if (Connection.bAvailable)
		{
			Connection.ServerName = Mcp.GetServerName();
			Connection.Url = Mcp.GetMcpUrl();
			Connection.AccessTokenHeader = Mcp.GetAccessTokenHeaderName();
			Connection.AccessToken = Mcp.GetAccessToken();
		}
		Backend->ConfigureMcpConnection(Connection);
	}
}

TSharedPtr<IWorldDataAgentBackend> FWorldDataAgentBackendFactory::CreateConfiguredBackend()
{
	const FString Selection = NormalizeSelection(ReadSelection());
	TSharedPtr<IWorldDataAgentBackend> Backend;
	if (Selection == TEXT("codex_app_server"))
	{
		Backend = MakeShared<FWorldDataCodexAppServerBackend>();
	}
	else if (Selection == TEXT("acp_codex"))
	{
		Backend = MakeShared<FWorldDataCodexACPClient>(TEXT("codex"));
	}
	else if (Selection == TEXT("acp_cursor"))
	{
		Backend = MakeShared<FWorldDataCodexACPClient>(TEXT("cursor"));
	}
	else if (Selection == TEXT("acp_claude"))
	{
		Backend = MakeShared<FWorldDataCodexACPClient>(TEXT("claude"));
	}
	else if (Selection != TEXT("auto"))
	{
		Backend = MakeShared<FUnavailableAgentBackend>(Selection, TEXT("Unknown Backend"), FString::Printf(TEXT("Unknown UEBridgeMCP Agent Backend '%s'. Use auto, codex_app_server, acp_codex, acp_cursor, or acp_claude."), *Selection));
	}
	else if (FWorldDataCodexAppServerBackend::IsConfigured())
	{
		Backend = MakeShared<FWorldDataCodexAppServerBackend>();
	}
	else
	{
		Backend = MakeShared<FWorldDataCodexACPClient>(TEXT("codex"));
	}
	ConfigureMcpConnection(Backend);
	return Backend;
}

FString FWorldDataAgentBackendFactory::GetConfiguredBackendId()
{
	return NormalizeSelection(ReadSelection());
}

TArray<FWorldDataAgentBackendOption> FWorldDataAgentBackendFactory::GetBackendOptions()
{
	const bool bAppServerConfigured = FWorldDataCodexAppServerBackend::IsConfigured();
	const bool bCodexAcpConfigured = FWorldDataCodexACPClient::IsProfileConfigured(TEXT("codex"));
	const bool bCursorAcpConfigured = FWorldDataCodexACPClient::IsProfileConfigured(TEXT("cursor"));
	const bool bClaudeAcpConfigured = FWorldDataCodexACPClient::IsProfileConfigured(TEXT("claude"));
	return {
		{ TEXT("auto"), TEXT("Auto"), TEXT("Prefer the pinned Codex app-server; otherwise use the pinned Codex ACP adapter."), bAppServerConfigured || bCodexAcpConfigured, false },
		{ TEXT("codex_app_server"), TEXT("Codex app-server"), TEXT("Official Codex JSONL backend. Model can be selected for a new thread."), bAppServerConfigured, true },
		{ TEXT("acp_codex"), TEXT("Codex ACP"), TEXT("Pinned ACP compatibility adapter for Codex."), bCodexAcpConfigured, false },
		{ TEXT("acp_cursor"), TEXT("Cursor ACP"), TEXT("Pinned ACP compatibility adapter for Cursor. This is not PATH or a Cursor CLI shim."), bCursorAcpConfigured, false },
		{ TEXT("acp_claude"), TEXT("Claude Code ACP"), TEXT("Pinned ACP compatibility adapter for Claude Code. This is not PATH or a Claude CLI shim."), bClaudeAcpConfigured, false }
	};
}

bool FWorldDataAgentBackendFactory::SetConfiguredBackendId(const FString& BackendId, FString& OutError)
{
	FString Normalized = BackendId;
	Normalized.TrimStartAndEndInline();
	Normalized.ToLowerInline();
	Normalized = NormalizeSelection(Normalized);
	if (!IsKnownSelection(Normalized))
	{
		OutError = FString::Printf(TEXT("Unknown Agent Backend '%s'."), *BackendId);
		return false;
	}

	if (!GConfig)
	{
		OutError = TEXT("UE configuration is unavailable.");
		return false;
	}
	GConfig->SetString(TEXT("UEBridgeMCP.Agent"), TEXT("Backend"), *Normalized, GGameIni);
	GConfig->Flush(false, GGameIni);
	return true;
}

FString FWorldDataAgentBackendFactory::GetCodexAppServerModel()
{
	return FWorldDataCodexAppServerBackend::GetConfiguredModel();
}

void FWorldDataAgentBackendFactory::SetCodexAppServerModel(const FString& Model)
{
	FWorldDataCodexAppServerBackend::SetConfiguredModel(Model);
}

FString FWorldDataAgentBackendFactory::GetDiagnosticsJson()
{
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	const FString Selection = NormalizeSelection(ReadSelection());
	Report->SetBoolField(TEXT("success"), true);
	Report->SetStringField(TEXT("configuredBackend"), Selection);
	Report->SetBoolField(TEXT("codexAppServerPinned"), FWorldDataCodexAppServerBackend::IsConfigured());
	Report->SetStringField(TEXT("autoResolution"), FWorldDataCodexAppServerBackend::IsConfigured() ? TEXT("codex_app_server") : TEXT("acp_codex"));
	Report->SetBoolField(TEXT("codexAcpPinned"), FWorldDataCodexACPClient::IsProfileConfigured(TEXT("codex")));
	Report->SetBoolField(TEXT("cursorAcpPinned"), FWorldDataCodexACPClient::IsProfileConfigured(TEXT("cursor")));
	Report->SetBoolField(TEXT("claudeAcpPinned"), FWorldDataCodexACPClient::IsProfileConfigured(TEXT("claude")));
	Report->SetStringField(TEXT("acpRole"), TEXT("Each ACP product is selected only through a separately pinned executable; protocol, model and attachment capabilities remain adapter-defined."));
	Report->SetStringField(TEXT("mcpBoundary"), TEXT("UE automation remains HTTP MCP; no backend receives an in-process MCP bridge."));
	return ToJson(Report);
}
