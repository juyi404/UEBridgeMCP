#pragma once

#include "CoreMinimal.h"
#include "UEBridgeMCPCoreModule.h"

class FUEBridgeMCPApprovalController
{
public:
	TArray<FWorldDataMCPApprovalSummary> GetPendingApprovals() const;
	bool Resolve(const FString& ApprovalId, bool bApprove, FString& OutError) const;
};
