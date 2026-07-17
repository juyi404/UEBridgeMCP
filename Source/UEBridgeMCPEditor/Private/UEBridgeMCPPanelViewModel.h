#pragma once

#include "CoreMinimal.h"

// UI-owned state only. It deliberately reaches Core through the exported
// service interface and never stores an MCP token, Session, Job, or approval.
class FUEBridgeMCPPanelViewModel
{
public:
	void Initialize(const FText& InitialAction);
	void SyncServerPort();

	FText LastAction;
	FString CurrentDetailText;
	FString ServerPortText;
};
