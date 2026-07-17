#pragma once

#include "CoreMinimal.h"
#include "WorldDataAgentBackend.h"

class FUEBridgeMCPAgentController
{
public:
	TSharedPtr<IWorldDataAgentBackend> Start(
		EWorldDataCodexPermissionMode PermissionMode,
		FWorldDataAcpTextDelegate OnText,
		FWorldDataAcpStatusDelegate OnStatus,
		FWorldDataAcpErrorDelegate OnError,
		FWorldDataAcpPermissionDelegate OnPermission);
	void Stop();

private:
	TSharedPtr<IWorldDataAgentBackend> Backend;
};
