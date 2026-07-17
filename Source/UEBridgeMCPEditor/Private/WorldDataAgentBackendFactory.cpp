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

	static FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	static void ConfigureMcpConnection(const TSharedPtr<IWorldDataAgentBackend>& Backend)
	{
		if (!Backend.IsValid() || Backend->GetBackendId() != TEXT("acp"))
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
	const FString Selection = ReadSelection();
	TSharedPtr<IWorldDataAgentBackend> Backend;
	if (Selection == TEXT("codex_app_server"))
	{
		Backend = MakeShared<FWorldDataCodexAppServerBackend>();
	}
	else if (Selection == TEXT("acp"))
	{
		Backend = MakeShared<FWorldDataCodexACPClient>();
	}
	else if (Selection == TEXT("cursor"))
	{
		Backend = MakeShared<FUnavailableAgentBackend>(TEXT("cursor"), TEXT("Cursor Backend"), TEXT("Cursor is not hosted as an ACP child process. Configure Cursor as an HTTP MCP client; a dedicated conversation backend has not been installed."));
	}
	else if (Selection == TEXT("claude"))
	{
		Backend = MakeShared<FUnavailableAgentBackend>(TEXT("claude"), TEXT("Claude Backend"), TEXT("Claude Code is not hosted as an ACP child process. Configure Claude Code as an HTTP MCP client; a dedicated conversation backend has not been installed."));
	}
	else if (Selection != TEXT("auto"))
	{
		Backend = MakeShared<FUnavailableAgentBackend>(Selection, TEXT("Unknown Backend"), FString::Printf(TEXT("Unknown UEBridgeMCP Agent Backend '%s'. Use auto, codex_app_server, acp, cursor, or claude."), *Selection));
	}
	else if (FWorldDataCodexAppServerBackend::IsConfigured())
	{
		Backend = MakeShared<FWorldDataCodexAppServerBackend>();
	}
	else
	{
		Backend = MakeShared<FWorldDataCodexACPClient>();
	}
	ConfigureMcpConnection(Backend);
	return Backend;
}

FString FWorldDataAgentBackendFactory::GetConfiguredBackendId()
{
	return ReadSelection();
}

FString FWorldDataAgentBackendFactory::GetDiagnosticsJson()
{
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	const FString Selection = ReadSelection();
	Report->SetBoolField(TEXT("success"), true);
	Report->SetStringField(TEXT("configuredBackend"), Selection);
	Report->SetBoolField(TEXT("codexAppServerPinned"), FWorldDataCodexAppServerBackend::IsConfigured());
	Report->SetStringField(TEXT("autoResolution"), FWorldDataCodexAppServerBackend::IsConfigured() ? TEXT("codex_app_server") : TEXT("acp"));
	Report->SetStringField(TEXT("acpRole"), TEXT("compatibility adapter only; executable protocol and capabilities are adapter-defined"));
	Report->SetStringField(TEXT("cursorRole"), TEXT("HTTP MCP client configuration, not an embedded conversation backend"));
	Report->SetStringField(TEXT("claudeRole"), TEXT("HTTP MCP client configuration, not an embedded conversation backend"));
	Report->SetStringField(TEXT("mcpBoundary"), TEXT("UE automation remains HTTP MCP; no backend receives an in-process MCP bridge."));
	return ToJson(Report);
}
