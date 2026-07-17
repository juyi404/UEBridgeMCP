#include "UEBridgeMCPApprovalController.h"

#include "UEBridgeMCPCoreModule.h"

TArray<FWorldDataMCPApprovalSummary> FUEBridgeMCPApprovalController::GetPendingApprovals() const
{
	return IUEBridgeMCPCoreModule::Get().GetService().GetPendingApprovals();
}

bool FUEBridgeMCPApprovalController::Resolve(const FString& ApprovalId, bool bApprove, FString& OutError) const
{
	return IUEBridgeMCPCoreModule::Get().GetService().ResolvePendingApproval(ApprovalId, bApprove, OutError);
}
