#pragma once

#include "CoreMinimal.h"
#include "WorldDataMCPServer.h"

// One redaction path for every embedded Agent Backend. It is intentionally
// applied before editor logs, UI transcript diagnostics, and stored errors.
namespace WorldDataAgentLogRedaction
{
	inline void RedactDelimitedValue(FString& Text, const FString& Key)
	{
		int32 SearchFrom = 0;
		FString Lower = Text.ToLower();
		const FString LowerKey = Key.ToLower();
		while (true)
		{
			const int32 KeyIndex = Lower.Find(LowerKey, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (KeyIndex == INDEX_NONE) return;
			int32 ValueStart = KeyIndex + Key.Len();
			while (ValueStart < Text.Len() && (FChar::IsWhitespace(Text[ValueStart]) || Text[ValueStart] == TEXT(':') || Text[ValueStart] == TEXT('='))) ++ValueStart;
			if (Text.Mid(ValueStart, 7).Equals(TEXT("bearer "), ESearchCase::IgnoreCase)) ValueStart += 7;
			const TCHAR Quote = ValueStart < Text.Len() && (Text[ValueStart] == TEXT('\"') || Text[ValueStart] == TEXT('\'')) ? Text[ValueStart++] : 0;
			int32 ValueEnd = ValueStart;
			while (ValueEnd < Text.Len() && (Quote ? Text[ValueEnd] != Quote : !FChar::IsWhitespace(Text[ValueEnd]) && Text[ValueEnd] != TEXT(',') && Text[ValueEnd] != TEXT('}') && Text[ValueEnd] != TEXT('&'))) ++ValueEnd;
			if (ValueEnd <= ValueStart)
			{
				SearchFrom = ValueStart + 1;
				continue;
			}
			Text = Text.Left(ValueStart) + TEXT("[REDACTED]") + Text.Mid(ValueEnd);
			Lower = Text.ToLower();
			SearchFrom = ValueStart + 10;
		}
	}

	inline FString Redact(FString Text)
	{
		const FString McpToken = FWorldDataMCPServer::GetAccessToken();
		if (!McpToken.IsEmpty()) Text.ReplaceInline(*McpToken, TEXT("[REDACTED_MCP_TOKEN]"), ESearchCase::CaseSensitive);
		for (const TCHAR* Key : { TEXT("authorization"), TEXT("access_token"), TEXT("token"), TEXT("api_key"), TEXT("password"), TEXT("secret") })
		{
			RedactDelimitedValue(Text, Key);
		}
		return Text;
	}
}
