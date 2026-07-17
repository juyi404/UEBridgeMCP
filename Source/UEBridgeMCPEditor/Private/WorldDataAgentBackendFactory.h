#pragma once

#include "CoreMinimal.h"

class IWorldDataAgentBackend;

struct FWorldDataAgentBackendOption
{
	FString Id;
	FString DisplayName;
	FString Description;
	bool bConfigured = false;
	bool bSupportsModelSelection = false;
};

class FWorldDataAgentBackendFactory
{
public:
	// backend: auto | codex_app_server | acp_codex | acp_cursor | acp_claude.
	// `auto` prefers a fully pinned official Codex app-server, then Codex ACP.
	static TSharedPtr<IWorldDataAgentBackend> CreateConfiguredBackend();
	static FString GetConfiguredBackendId();
	static TArray<FWorldDataAgentBackendOption> GetBackendOptions();
	static bool SetConfiguredBackendId(const FString& BackendId, FString& OutError);
	static FString GetCodexAppServerModel();
	static void SetCodexAppServerModel(const FString& Model);
	static FString GetDiagnosticsJson();
};
