#pragma once

#include "CoreMinimal.h"

class IWorldDataAgentBackend;

class FWorldDataAgentBackendFactory
{
public:
	// backend: auto | codex_app_server | acp | cursor | claude
	// `auto` prefers a fully pinned official Codex app-server, then ACP.
	static TSharedPtr<IWorldDataAgentBackend> CreateConfiguredBackend();
	static FString GetConfiguredBackendId();
	static FString GetDiagnosticsJson();
};
