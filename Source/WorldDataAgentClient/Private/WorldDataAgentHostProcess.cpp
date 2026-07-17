#include "WorldDataAgentHostProcess.h"

#include "HAL/RunnableThread.h"
#include "Containers/StringConv.h"
#include "WorldDataAgentTypes.h"

FWorldDataAgentHostProcess::FWorldDataAgentHostProcess() = default;

FWorldDataAgentHostProcess::~FWorldDataAgentHostProcess()
{
	RequestStop(true);
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	CloseResources();
}

bool FWorldDataAgentHostProcess::Launch(
	const FString& Executable,
	const FString& WorkingDirectory)
{
	if (bRunning.Load() || ProcessHandle.IsValid()) return false;

	void* StdoutWriteChild = nullptr;
	void* StdinReadChild = nullptr;
	void* StderrWriteChild = nullptr;
	if (!FPlatformProcess::CreatePipe(StdoutReadParent, StdoutWriteChild)
		|| !FPlatformProcess::CreatePipe(StdinReadChild, StdinWriteParent, true)
		|| !FPlatformProcess::CreatePipe(StderrReadParent, StderrWriteChild))
	{
		FPlatformProcess::ClosePipe(StdoutReadParent, StdoutWriteChild);
		FPlatformProcess::ClosePipe(StdinReadChild, StdinWriteParent);
		FPlatformProcess::ClosePipe(StderrReadParent, StderrWriteChild);
		StdoutReadParent = StdinWriteParent = StderrReadParent = nullptr;
		return false;
	}

	ProcessHandle = FPlatformProcess::CreateProc(
		*Executable,
		TEXT(""),
		false,
		true,
		true,
		nullptr,
		0,
		WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
		StdoutWriteChild,
		StdinReadChild,
		StderrWriteChild);

	// The parent must not retain the inheritable child ends.
	FPlatformProcess::ClosePipe(nullptr, StdoutWriteChild);
	FPlatformProcess::ClosePipe(StdinReadChild, nullptr);
	FPlatformProcess::ClosePipe(nullptr, StderrWriteChild);

	if (!ProcessHandle.IsValid())
	{
		CloseResources();
		return false;
	}

	bStopRequested.Store(false);
	bKillTree.Store(false);
	bRunning.Store(true);
	Thread = FRunnableThread::Create(this, TEXT("WorldDataAgentHostProcess"));
	if (!Thread)
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
		bRunning.Store(false);
		CloseResources();
		return false;
	}
	return true;
}

bool FWorldDataAgentHostProcess::SendJsonLine(const FString& Json)
{
	if (!bRunning.Load() || Json.IsEmpty()) return false;
	FTCHARToUTF8 Converted(*Json);
	if (Converted.Length() > WorldDataAgentProtocol::MaximumOutboundPayloadBytes) return false;
	TArray<uint8> Frame;
	Frame.Reserve(Converted.Length() + 1);
	Frame.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	Frame.Add(static_cast<uint8>('\n'));
	PendingInput.Enqueue(MoveTemp(Frame));
	return true;
}

void FWorldDataAgentHostProcess::RequestStop(const bool bKillProcessTree)
{
	bKillTree.Store(bKillProcessTree);
	bStopRequested.Store(true);
}

bool FWorldDataAgentHostProcess::IsRunning() const
{
	return bRunning.Load();
}

uint32 FWorldDataAgentHostProcess::Run()
{
	while (ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcessHandle))
	{
		DispatchAvailableOutput();
		FlushPendingInput();
		if (bStopRequested.Load())
		{
			FPlatformProcess::TerminateProc(ProcessHandle, bKillTree.Load());
			break;
		}
		FPlatformProcess::SleepNoStats(0.005f);
	}

	DispatchAvailableOutput();
	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
	const bool bCancelled = bStopRequested.Load();
	bRunning.Store(false);
	CompletedDelegate.ExecuteIfBound(ReturnCode, bCancelled);
	return 0;
}

void FWorldDataAgentHostProcess::Stop()
{
	RequestStop(false);
}

void FWorldDataAgentHostProcess::CloseResources()
{
	FPlatformProcess::ClosePipe(StdoutReadParent, nullptr);
	FPlatformProcess::ClosePipe(nullptr, StdinWriteParent);
	FPlatformProcess::ClosePipe(StderrReadParent, nullptr);
	StdoutReadParent = StdinWriteParent = StderrReadParent = nullptr;
	if (ProcessHandle.IsValid()) FPlatformProcess::CloseProc(ProcessHandle);
}

void FWorldDataAgentHostProcess::DispatchAvailableOutput()
{
	TArray<uint8> Bytes;
	if (StdoutReadParent && FPlatformProcess::ReadPipeToArray(StdoutReadParent, Bytes) && !Bytes.IsEmpty())
	{
		StdoutDelegate.ExecuteIfBound(Bytes);
	}
	Bytes.Reset();
	if (StderrReadParent && FPlatformProcess::ReadPipeToArray(StderrReadParent, Bytes) && !Bytes.IsEmpty())
	{
		StderrDelegate.ExecuteIfBound(Bytes);
	}
}

void FWorldDataAgentHostProcess::FlushPendingInput()
{
	TArray<uint8> Frame;
	while (PendingInput.Dequeue(Frame))
	{
		int32 Written = 0;
		if (!FPlatformProcess::WritePipe(StdinWriteParent, Frame.GetData(), Frame.Num(), &Written) || Written != Frame.Num())
		{
			RequestStop(false);
			return;
		}
	}
}
