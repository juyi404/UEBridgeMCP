#if WITH_DEV_AUTOMATION_TESTS

#include "WorldDataMCPServer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonObject> MakeRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Request->SetNumberField(TEXT("id"), 1);
		Request->SetStringField(TEXT("method"), Method);
		Request->SetObjectField(TEXT("params"), Params.IsValid() ? Params.ToSharedRef() : MakeShared<FJsonObject>());
		return Request;
	}

	TSharedPtr<FJsonObject> GetResult(const TSharedPtr<FJsonObject>& Response)
	{
		const TSharedPtr<FJsonObject>* Result = nullptr;
		return Response.IsValid() && Response->TryGetObjectField(TEXT("result"), Result) && Result && Result->IsValid() ? *Result : nullptr;
	}

	bool ParseObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEBridgeMCPJsonRpcContractTest,
	"UEBridgeMCP.Contract.JsonRpcCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEBridgeMCPJsonRpcContractTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
	InitializeParams->SetStringField(TEXT("protocolVersion"), TEXT("2025-06-18"));
	TSharedPtr<FJsonObject> Initialize = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("initialize"), InitializeParams)));
	TestTrue(TEXT("initialize returns a JSON-RPC result"), Initialize.IsValid());

	TSharedPtr<FJsonObject> ToolList = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("tools/list"), MakeShared<FJsonObject>())));
	TestTrue(TEXT("tools/list returns a result"), ToolList.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	TestTrue(TEXT("tools/list exposes a tools array"), ToolList.IsValid() && ToolList->TryGetArrayField(TEXT("tools"), Tools) && Tools && Tools->Num() > 0);
	bool bHasLegacyResourceTool = false;
	bool bHasReadLog = false;
	if (Tools)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tools)
		{
			FString Name;
			const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Tool.IsValid() && Tool->TryGetStringField(TEXT("name"), Name))
			{
				bHasLegacyResourceTool |= Name == TEXT("list_resources") || Name == TEXT("read_resource");
				bHasReadLog |= Name == TEXT("read_log");
			}
		}
	}
	TestFalse(TEXT("tools/list does not expose legacy resource tools"), bHasLegacyResourceTool);
	TestTrue(TEXT("tools/list retains migrated supplemental tools"), bHasReadLog);

	TSharedPtr<FJsonObject> ResourceList = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("resources/list"), MakeShared<FJsonObject>())));
	TestTrue(TEXT("resources/list returns a result"), ResourceList.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* Resources = nullptr;
	bool bHasLegacyUri = false;
	if (ResourceList.IsValid() && ResourceList->TryGetArrayField(TEXT("resources"), Resources) && Resources)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Resources)
		{
			FString Uri;
			const TSharedPtr<FJsonObject> Resource = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Resource.IsValid() && Resource->TryGetStringField(TEXT("uri"), Uri))
			{
				bHasLegacyUri |= Uri.StartsWith(TEXT("ubridge://"));
			}
		}
	}
	TestFalse(TEXT("resources/list does not expose legacy ubridge URIs"), bHasLegacyUri);

	TSharedRef<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("uri"), TEXT("worlddata://mcp/governance"));
	TSharedPtr<FJsonObject> ResourceRead = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("resources/read"), ReadParams)));
	TestTrue(TEXT("resources/read returns a result"), ResourceRead.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEBridgeMCPContextPreconditionTest,
	"UEBridgeMCP.Contract.ContextPrecondition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEBridgeMCPContextPreconditionTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> GovernanceCall = MakeShared<FJsonObject>();
	GovernanceCall->SetStringField(TEXT("name"), TEXT("get_mcp_governance"));
	GovernanceCall->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> GovernanceResult = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("tools/call"), GovernanceCall)));
	TestTrue(TEXT("read-only tools/call returns a result"), GovernanceResult.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (GovernanceResult.IsValid() && GovernanceResult->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0)
	{
		const TSharedPtr<FJsonObject> TextItem = (*Content)[0]->AsObject();
		FString Text;
		TSharedPtr<FJsonObject> ToolPayload;
		TestTrue(TEXT("tool result is structured JSON text"), TextItem.IsValid() && TextItem->TryGetStringField(TEXT("text"), Text) && ParseObject(Text, ToolPayload));
		if (ToolPayload.IsValid())
		{
			const TSharedPtr<FJsonObject>* Envelope = nullptr;
			TestTrue(TEXT("every tool result carries ContextEnvelope"), ToolPayload->TryGetObjectField(TEXT("contextEnvelope"), Envelope) && Envelope && Envelope->IsValid());
		}
	}

	TSharedRef<FJsonObject> UnsafeWrite = MakeShared<FJsonObject>();
	UnsafeWrite->SetStringField(TEXT("name"), TEXT("save_current_level"));
	UnsafeWrite->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> RejectedWrite = GetResult(FWorldDataMCPServer::ProcessJsonRpc(MakeRequest(TEXT("tools/call"), UnsafeWrite)));
	bool bIsError = false;
	TestTrue(TEXT("writes without a fresh ContextEnvelope are rejected"), RejectedWrite.IsValid() && RejectedWrite->TryGetBoolField(TEXT("isError"), bIsError) && bIsError);
	return true;
}

#endif
