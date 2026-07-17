#include "UEBridgeMCPPanelViewModel.h"

#include "UEBridgeMCPCoreModule.h"

void FUEBridgeMCPPanelViewModel::Initialize(const FText& InitialAction)
{
	LastAction = InitialAction;
	SyncServerPort();
}

void FUEBridgeMCPPanelViewModel::SyncServerPort()
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	ServerPortText = FString::FromInt(Service.IsRunning() ? Service.GetPort() : Service.LoadConfiguredPort());
}
