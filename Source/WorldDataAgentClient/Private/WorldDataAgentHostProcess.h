#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/PlatformProcess.h"

class FWorldDataAgentHostProcess final : public FRunnable
{
public:
	DECLARE_DELEGATE_OneParam(FOutputDelegate, const TArray<uint8>&);
	DECLARE_DELEGATE_TwoParams(FCompletedDelegate, int32, bool);

	FWorldDataAgentHostProcess();
	virtual ~FWorldDataAgentHostProcess() override;

	bool Launch(
		const FString& Executable,
		const FString& WorkingDirectory);
	bool SendJsonLine(const FString& Json);
	void RequestStop(bool bKillProcessTree);
	bool IsRunning() const;

	FOutputDelegate& OnStdout() { return StdoutDelegate; }
	FOutputDelegate& OnStderr() { return StderrDelegate; }
	FCompletedDelegate& OnCompleted() { return CompletedDelegate; }

	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	void CloseResources();
	void DispatchAvailableOutput();
	void FlushPendingInput();

	FProcHandle ProcessHandle;
	FRunnableThread* Thread = nullptr;
	void* StdoutReadParent = nullptr;
	void* StdinWriteParent = nullptr;
	void* StderrReadParent = nullptr;
	TQueue<TArray<uint8>, EQueueMode::Mpsc> PendingInput;
	TAtomic<bool> bRunning = false;
	TAtomic<bool> bStopRequested = false;
	TAtomic<bool> bKillTree = false;
	FOutputDelegate StdoutDelegate;
	FOutputDelegate StderrDelegate;
	FCompletedDelegate CompletedDelegate;
};
