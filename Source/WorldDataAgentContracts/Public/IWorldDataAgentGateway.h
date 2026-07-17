#pragma once

#include "WorldDataAgentTypes.h"

class IWorldDataAgentGateway
{
public:
	virtual ~IWorldDataAgentGateway() = default;

	virtual void Connect(const FWorldDataAgentConnectionOptions& Options) = 0;
	virtual void Disconnect() = 0;
	virtual FString ListThreads(const FWorldDataListThreadsRequest& Request) = 0;
	virtual FString CreateThread(const FWorldDataCreateThreadRequest& Request) = 0;
	virtual FString ResumeThread(const FWorldDataResumeThreadRequest& Request) = 0;
	virtual FString SendTurn(const FWorldDataSendTurnRequest& Request) = 0;
	virtual void InterruptTurn(const FString& ThreadId, const FString& TurnId) = 0;
	virtual void ResolveApproval(const FWorldDataApprovalDecision& Decision) = 0;
	virtual FWorldDataAgentStatusSnapshot GetStatus() const = 0;
	virtual FWorldDataAgentEventDelegate& OnEvent() = 0;
};
