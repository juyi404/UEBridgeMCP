#include "UEBridgeMCPAgentController.h"

#include "WorldDataAgentBackendFactory.h"

TSharedPtr<IWorldDataAgentBackend> FUEBridgeMCPAgentController::Start(
	EWorldDataCodexPermissionMode PermissionMode,
	FWorldDataAcpTextDelegate OnText,
	FWorldDataAcpStatusDelegate OnStatus,
	FWorldDataAcpErrorDelegate OnError,
	FWorldDataAcpPermissionDelegate OnPermission)
{
	Stop();
	Backend = FWorldDataAgentBackendFactory::CreateConfiguredBackend();
	if (Backend.IsValid())
	{
		Backend->SetPermissionMode(PermissionMode);
		Backend->OnText = MoveTemp(OnText);
		Backend->OnStatus = MoveTemp(OnStatus);
		Backend->OnError = MoveTemp(OnError);
		Backend->OnPermission = MoveTemp(OnPermission);
	}
	return Backend;
}

void FUEBridgeMCPAgentController::Stop()
{
	if (Backend.IsValid())
	{
		Backend->Stop();
	}
	Backend.Reset();
}
