#include "WorldDataMCPKnowledgeTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPPcgKnowledgeTools.h"
#include "WorldDataMCPSpatialTools.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"

namespace WorldDataMCP
{
namespace KnowledgeTools
{
namespace
{
	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return GWorld;
	}

	// Blocking HTTP from a game-thread-marshaled tool: fire the request and manually tick the
	// HTTP manager until it completes or the timeout elapses (the normal game-thread tick is
	// paused while we block here, so we must pump it ourselves). Avoids the async/sync deadlock.
	bool HttpRequestSync(const FString& Url, const FString& Verb,
		const TArray<TPair<FString, FString>>& Headers, const FString& Body,
		float TimeoutSec, int32& OutCode, FString& OutResponse)
	{
		FHttpModule& Http = FHttpModule::Get();
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Http.CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		for (const TPair<FString, FString>& Header : Headers)
		{
			Request->SetHeader(Header.Key, Header.Value);
		}
		if (!Body.IsEmpty())
		{
			Request->SetContentAsString(Body);
		}

		bool bDone = false;
		bool bSucceededOut = false;
		int32 Code = 0;
		FString Response;
		Request->OnProcessRequestComplete().BindLambda(
			[&bDone, &bSucceededOut, &Code, &Response]
			(FHttpRequestPtr, FHttpResponsePtr Res, bool bSucceeded)
			{
				bSucceededOut = bSucceeded && Res.IsValid();
				if (Res.IsValid())
				{
					Code = Res->GetResponseCode();
					Response = Res->GetContentAsString();
				}
				bDone = true;
			});

		Request->ProcessRequest();
		const double Start = FPlatformTime::Seconds();
		while (!bDone && (FPlatformTime::Seconds() - Start) < TimeoutSec)
		{
			Http.GetHttpManager().Tick(0.05f);
			FPlatformProcess::Sleep(0.02f);
		}
		if (!bDone)
		{
			Request->CancelRequest();
			return false;
		}
		OutCode = Code;
		OutResponse = Response;
		return bSucceededOut;
	}

	// Crude HTML -> text: drop <script>/<style> blocks and all tags, collapse whitespace,
	// decode a few common entities. Good enough to feed doc prose to the agent.
	FString HtmlToText(const FString& Html)
	{
		FString Text = Html;

		auto RemoveBlock = [&Text](const TCHAR* OpenTag, const TCHAR* CloseTag)
		{
			int32 Start = 0;
			while ((Start = Text.Find(OpenTag, ESearchCase::IgnoreCase)) != INDEX_NONE)
			{
				const int32 End = Text.Find(CloseTag, ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
				if (End == INDEX_NONE)
				{
					Text.LeftInline(Start);
					break;
				}
				Text.RemoveAt(Start, End + FCString::Strlen(CloseTag) - Start, EAllowShrinking::No);
			}
		};
		RemoveBlock(TEXT("<script"), TEXT("</script>"));
		RemoveBlock(TEXT("<style"), TEXT("</style>"));

		// Strip remaining tags.
		FString Out;
		Out.Reserve(Text.Len());
		bool bInTag = false;
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			const TCHAR C = Text[i];
			if (C == TEXT('<')) { bInTag = true; continue; }
			if (C == TEXT('>')) { bInTag = false; Out.AppendChar(TEXT(' ')); continue; }
			if (!bInTag) { Out.AppendChar(C); }
		}

		Out.ReplaceInline(TEXT("&nbsp;"), TEXT(" "));
		Out.ReplaceInline(TEXT("&amp;"), TEXT("&"));
		Out.ReplaceInline(TEXT("&lt;"), TEXT("<"));
		Out.ReplaceInline(TEXT("&gt;"), TEXT(">"));
		Out.ReplaceInline(TEXT("&quot;"), TEXT("\""));
		Out.ReplaceInline(TEXT("&#39;"), TEXT("'"));

		// Collapse runs of whitespace/newlines.
		FString Collapsed;
		Collapsed.Reserve(Out.Len());
		bool bPrevSpace = false;
		for (int32 i = 0; i < Out.Len(); ++i)
		{
			TCHAR C = Out[i];
			const bool bSpace = FChar::IsWhitespace(C);
			if (bSpace)
			{
				if (!bPrevSpace) { Collapsed.AppendChar(TEXT(' ')); }
				bPrevSpace = true;
			}
			else
			{
				Collapsed.AppendChar(C);
				bPrevSpace = false;
			}
		}
		return Collapsed.TrimStartAndEnd();
	}

	// ---- A: runtime knowledge retrieval -------------------------------------------------

	FString FetchDoc(const TSharedPtr<FJsonObject>& Args)
	{
		FString Url;
		if (!Args->TryGetStringField(TEXT("url"), Url) || Url.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'url'."));
		}
		double MaxBytesNumber = 20000.0;
		Args->TryGetNumberField(TEXT("maxBytes"), MaxBytesNumber);
		const int32 MaxBytes = FMath::Clamp(static_cast<int32>(MaxBytesNumber), 500, 200000);

		int32 Code = 0;
		FString Body;
		const bool bOk = HttpRequestSync(Url, TEXT("GET"), {}, FString(), 20.0f, Code, Body);
		if (!bOk)
		{
			return ErrorJson(FString::Printf(TEXT("HTTP GET failed for '%s' (code %d)."), *Url, Code));
		}
		FString Text = HtmlToText(Body);
		bool bTruncated = false;
		if (Text.Len() > MaxBytes)
		{
			Text.LeftInline(MaxBytes);
			bTruncated = true;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("url"), Url);
		Result->SetNumberField(TEXT("statusCode"), Code);
		Result->SetBoolField(TEXT("truncated"), bTruncated);
		Result->SetStringField(TEXT("text"), Text);
		return SuccessJson(Result);
	}

	FString WebSearch(const TSharedPtr<FJsonObject>& Args)
	{
		FString Query;
		if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'query'."));
		}
		// Bias toward Unreal Engine results unless the caller opts out or already mentions UE.
		bool bRaw = false;
		Args->TryGetBoolField(TEXT("raw"), bRaw);
		if (!bRaw && !Query.Contains(TEXT("Unreal"), ESearchCase::IgnoreCase))
		{
			Query += TEXT(" Unreal Engine 5");
		}
		double MaxResultsNumber = 5.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 15);

		FString Provider = FPlatformMisc::GetEnvironmentVariable(TEXT("UEMCP_SEARCH_PROVIDER"));
		if (Provider.IsEmpty())
		{
			Provider = TEXT("tavily");
		}
		const FString ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("UEMCP_SEARCH_API_KEY"));
		if (ApiKey.IsEmpty())
		{
			return ErrorJson(TEXT("No search API key. Set env var UEMCP_SEARCH_API_KEY (and optionally UEMCP_SEARCH_PROVIDER=tavily|brave). For keyless reads, use ue_fetch_doc with a known URL."));
		}

		int32 Code = 0;
		FString Response;
		TArray<TSharedPtr<FJsonValue>> Results;

		if (Provider.Equals(TEXT("brave"), ESearchCase::IgnoreCase))
		{
			const FString Url = FString::Printf(TEXT("https://api.search.brave.com/res/v1/web/search?count=%d&q=%s"),
				MaxResults, *FGenericPlatformHttp::UrlEncode(Query));
			TArray<TPair<FString, FString>> Headers;
			Headers.Add({ TEXT("Accept"), TEXT("application/json") });
			Headers.Add({ TEXT("X-Subscription-Token"), ApiKey });
			if (!HttpRequestSync(Url, TEXT("GET"), Headers, FString(), 20.0f, Code, Response))
			{
				return ErrorJson(FString::Printf(TEXT("Brave search failed (code %d)."), Code));
			}
			TSharedPtr<FJsonObject> Json = ParseJsonObject(Response);
			const TArray<TSharedPtr<FJsonValue>>* WebResults = nullptr;
			const TSharedPtr<FJsonObject>* Web = nullptr;
			if (Json.IsValid() && Json->TryGetObjectField(TEXT("web"), Web) && Web
				&& (*Web)->TryGetArrayField(TEXT("results"), WebResults) && WebResults)
			{
				for (const TSharedPtr<FJsonValue>& Item : *WebResults)
				{
					const TSharedPtr<FJsonObject> Obj = Item->AsObject();
					if (!Obj.IsValid()) { continue; }
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("title"), Obj->GetStringField(TEXT("title")));
					Entry->SetStringField(TEXT("url"), Obj->GetStringField(TEXT("url")));
					FString Desc;
					Obj->TryGetStringField(TEXT("description"), Desc);
					Entry->SetStringField(TEXT("snippet"), Desc);
					Results.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}
		else // tavily
		{
			const FString Url = TEXT("https://api.tavily.com/search");
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("api_key"), ApiKey);
			Payload->SetStringField(TEXT("query"), Query);
			Payload->SetNumberField(TEXT("max_results"), MaxResults);
			Payload->SetStringField(TEXT("search_depth"), TEXT("basic"));
			Payload->SetBoolField(TEXT("include_answer"), true);
			const FString Body = JsonObjectToString(Payload);
			TArray<TPair<FString, FString>> Headers;
			Headers.Add({ TEXT("Content-Type"), TEXT("application/json") });
			if (!HttpRequestSync(Url, TEXT("POST"), Headers, Body, 25.0f, Code, Response))
			{
				return ErrorJson(FString::Printf(TEXT("Tavily search failed (code %d)."), Code));
			}
			TSharedPtr<FJsonObject> Json = ParseJsonObject(Response);
			if (Json.IsValid())
			{
				FString Answer;
				if (Json->TryGetStringField(TEXT("answer"), Answer) && !Answer.IsEmpty())
				{
					TSharedRef<FJsonObject> AnswerEntry = MakeShared<FJsonObject>();
					AnswerEntry->SetStringField(TEXT("title"), TEXT("Synthesized answer"));
					AnswerEntry->SetStringField(TEXT("snippet"), Answer);
					Results.Add(MakeShared<FJsonValueObject>(AnswerEntry));
				}
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (Json->TryGetArrayField(TEXT("results"), Items) && Items)
				{
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						const TSharedPtr<FJsonObject> Obj = Item->AsObject();
						if (!Obj.IsValid()) { continue; }
						TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("title"), Obj->GetStringField(TEXT("title")));
						Entry->SetStringField(TEXT("url"), Obj->GetStringField(TEXT("url")));
						FString Content;
						Obj->TryGetStringField(TEXT("content"), Content);
						Entry->SetStringField(TEXT("snippet"), Content);
						Results.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("query"), Query);
		Result->SetStringField(TEXT("provider"), Provider);
		Result->SetNumberField(TEXT("count"), Results.Num());
		Result->SetArrayField(TEXT("results"), Results);
		Result->SetStringField(TEXT("hint"), TEXT("Use ue_fetch_doc on a result URL to read the full page before acting."));
		return SuccessJson(Result);
	}

	// ---- B: driver-aware scene readout --------------------------------------------------

	FString DescribeScene(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("level"), World->GetMapName());

		// What drives the lighting/look — the levers an agent should reach for.
		TArray<TSharedPtr<FJsonValue>> DirectionalLights;
		bool bHasSkyAtmosphere = false;
		bool bHasFog = false;
		double FogDensity = 0.0;
		bool bHasSkyLight = false;
		FString SkyLightMobility;
		int32 PostProcessVolumes = 0;

		// Spatial structure — what turns a flat actor dump into an understanding of layout:
		// overall world bounds, a semantic category histogram, and the largest landmarks.
		// Regions (dense clusters) are computed below once the final bounds are known.
		FBox LevelBounds(ForceInit);
		int32 SpatialActorCount = 0;
		TMap<FString, int32> CategoryCounts;
		struct FLandmarkCandidate { double Footprint; FString Label; FString Category; FVector Center; };
		TArray<FLandmarkCandidate> Landmarks;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}

			const FBox ActorBounds = SpatialTools::GetActorWorldBounds(Actor);
			if (SpatialTools::IsSpatiallyMeaningfulActor(Actor, ActorBounds))
			{
				LevelBounds += ActorBounds;
				++SpatialActorCount;
				const FString Category = SpatialTools::ClassifyActor(Actor);
				CategoryCounts.FindOrAdd(Category)++;
				const FVector BoundsSize = ActorBounds.GetSize();
				Landmarks.Add({ static_cast<double>(BoundsSize.X) * BoundsSize.Y, Actor->GetActorLabel(), Category, ActorBounds.GetCenter() });
			}

			if (UDirectionalLightComponent* Dir = Actor->FindComponentByClass<UDirectionalLightComponent>())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("actor"), Actor->GetActorLabel());
				const FRotator Rot = Actor->GetActorRotation();
				Entry->SetNumberField(TEXT("pitch"), Rot.Pitch);
				Entry->SetNumberField(TEXT("yaw"), Rot.Yaw);
				Entry->SetNumberField(TEXT("intensity"), Dir->Intensity);
				Entry->SetBoolField(TEXT("atmosphereSunLight"), Dir->IsUsedAsAtmosphereSunLight());
				Entry->SetStringField(TEXT("hint"), TEXT("Sun angle (pitch) drives time-of-day; low pitch = sunrise/sunset. Must be Atmosphere Sun Light to colour the sky."));
				DirectionalLights.Add(MakeShared<FJsonValueObject>(Entry));
			}
			if (Actor->FindComponentByClass<USkyAtmosphereComponent>())
			{
				bHasSkyAtmosphere = true;
			}
			if (UExponentialHeightFogComponent* Fog = Actor->FindComponentByClass<UExponentialHeightFogComponent>())
			{
				bHasFog = true;
				FogDensity = Fog->FogDensity;
			}
			if (USkyLightComponent* Sky = Actor->FindComponentByClass<USkyLightComponent>())
			{
				bHasSkyLight = true;
				switch (Sky->Mobility)
				{
				case EComponentMobility::Static: SkyLightMobility = TEXT("Static"); break;
				case EComponentMobility::Stationary: SkyLightMobility = TEXT("Stationary"); break;
				case EComponentMobility::Movable: SkyLightMobility = TEXT("Movable"); break;
				default: break;
				}
			}
			if (Actor->IsA<APostProcessVolume>())
			{
				++PostProcessVolumes;
			}
		}

		// Fold instanced scatter (PCG-ISM / foliage-HISM / lay-spline ISM) into the category
		// histogram. These live as sub-actor instances the loop above cannot see, so without this
		// the digest is blind to exactly the procedural content this project generates the most of.
		int32 InstanceCount = 0;
		SpatialTools::ForEachWorldInstance(World, [&](const AActor* /*Owner*/, int32 /*Index*/, const FBox& /*WB*/, const FString& Category)
		{
			CategoryCounts.FindOrAdd(Category)++;
			++InstanceCount;
		});

		TSharedRef<FJsonObject> Lighting = MakeShared<FJsonObject>();
		Lighting->SetArrayField(TEXT("directionalLights"), DirectionalLights);
		Lighting->SetBoolField(TEXT("hasSkyAtmosphere"), bHasSkyAtmosphere);
		Lighting->SetBoolField(TEXT("hasExponentialHeightFog"), bHasFog);
		if (bHasFog)
		{
			Lighting->SetNumberField(TEXT("fogDensity"), FogDensity);
		}
		Lighting->SetBoolField(TEXT("hasSkyLight"), bHasSkyLight);
		if (bHasSkyLight)
		{
			Lighting->SetStringField(TEXT("skyLightMobility"), SkyLightMobility);
		}
		Lighting->SetNumberField(TEXT("postProcessVolumes"), PostProcessVolumes);
		Result->SetObjectField(TEXT("lighting"), Lighting);

		// ---- Spatial digest -------------------------------------------------------------------
		Result->SetNumberField(TEXT("spatialActorCount"), SpatialActorCount);
		Result->SetNumberField(TEXT("instanceCount"), InstanceCount);
		if (LevelBounds.IsValid)
		{
			Result->SetObjectField(TEXT("bounds"), SpatialTools::BoxToJson(LevelBounds));
		}

		CategoryCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });
		TArray<TSharedPtr<FJsonValue>> CategoryArray;
		for (const TPair<FString, int32>& Pair : CategoryCounts)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("category"), Pair.Key);
			Entry->SetNumberField(TEXT("count"), Pair.Value);
			CategoryArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("categories"), CategoryArray);

		// Dense clusters of the world, each labelled with its dominant category — the level read
		// at an altitude an agent can actually reason about (a dozen regions, not a thousand actors).
		Result->SetArrayField(TEXT("regions"), SpatialTools::BuildRegionSummaries(World, LevelBounds, 4, 8));

		// The biggest few footprints — usually the landscape, road network, key structures.
		Landmarks.Sort([](const FLandmarkCandidate& A, const FLandmarkCandidate& B) { return A.Footprint > B.Footprint; });
		TArray<TSharedPtr<FJsonValue>> LandmarkArray;
		for (int32 Index = 0; Index < Landmarks.Num() && Index < 5; ++Index)
		{
			const FLandmarkCandidate& Landmark = Landmarks[Index];
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("label"), Landmark.Label);
			Entry->SetStringField(TEXT("category"), Landmark.Category);
			TArray<TSharedPtr<FJsonValue>> CenterArray;
			CenterArray.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Landmark.Center.X)));
			CenterArray.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Landmark.Center.Y)));
			CenterArray.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Landmark.Center.Z)));
			Entry->SetArrayField(TEXT("center"), CenterArray);
			LandmarkArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("landmarks"), LandmarkArray);

		Result->SetStringField(TEXT("loopHint"),
			TEXT("Perceive->act->perceive: read this, look up the correct approach with ue_web_search if unsure, apply changes (e.g. set_light_properties on the atmosphere sun light), then capture_viewport and compare to the goal."));
		return SuccessJson(Result);
	}

	// ---- LLM reasoning (server-side "thinking") -----------------------------------------

	FString ResolveModel(const TSharedPtr<FJsonObject>& Args)
	{
		FString Model;
		if (Args.IsValid() && Args->TryGetStringField(TEXT("model"), Model) && !Model.IsEmpty())
		{
			return Model;
		}
		Model = FPlatformMisc::GetEnvironmentVariable(TEXT("UEMCP_LLM_MODEL"));
		return Model.IsEmpty() ? FString(TEXT("claude-sonnet-4-6")) : Model;
	}

	// Call the Anthropic Messages API synchronously (reuses the game-thread-safe HTTP pump).
	// Blocks the editor for the duration of the LLM call — acceptable for a deliberate think step.
	bool CallAnthropic(const FString& System, const FString& User, const FString& Model, int32 MaxTokens, FString& OutText, FString& OutError)
	{
		const FString ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
		if (ApiKey.IsEmpty())
		{
			OutError = TEXT("No LLM API key. Set env var ANTHROPIC_API_KEY (model via UEMCP_LLM_MODEL, default claude-sonnet-4-6).");
			return false;
		}

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Model);
		Body->SetNumberField(TEXT("max_tokens"), MaxTokens);
		if (!System.IsEmpty())
		{
			Body->SetStringField(TEXT("system"), System);
		}
		TArray<TSharedPtr<FJsonValue>> Messages;
		TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("user"));
		Msg->SetStringField(TEXT("content"), User);
		Messages.Add(MakeShared<FJsonValueObject>(Msg));
		Body->SetArrayField(TEXT("messages"), Messages);

		TArray<TPair<FString, FString>> Headers;
		Headers.Add({ TEXT("content-type"), TEXT("application/json") });
		Headers.Add({ TEXT("x-api-key"), ApiKey });
		Headers.Add({ TEXT("anthropic-version"), TEXT("2023-06-01") });

		int32 Code = 0;
		FString Response;
		if (!HttpRequestSync(TEXT("https://api.anthropic.com/v1/messages"), TEXT("POST"), Headers, JsonObjectToString(Body), 90.0f, Code, Response))
		{
			OutError = FString::Printf(TEXT("LLM request failed/timeout (code %d)."), Code);
			return false;
		}
		if (Code != 200)
		{
			OutError = FString::Printf(TEXT("LLM API error (HTTP %d): %s"), Code, *Response.Left(600));
			return false;
		}
		TSharedPtr<FJsonObject> Json = ParseJsonObject(Response);
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (Json.IsValid() && Json->TryGetArrayField(TEXT("content"), Content) && Content)
		{
			FString Text;
			for (const TSharedPtr<FJsonValue>& Block : *Content)
			{
				const TSharedPtr<FJsonObject> Obj = Block->AsObject();
				FString T;
				if (Obj.IsValid() && Obj->TryGetStringField(TEXT("text"), T))
				{
					Text += T;
				}
			}
			OutText = Text;
			return true;
		}
		OutError = TEXT("LLM response had no text content.");
		return false;
	}

	FString LlmThink(const TSharedPtr<FJsonObject>& Args)
	{
		FString Prompt;
		if (!Args->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'prompt'."));
		}
		FString System;
		Args->TryGetStringField(TEXT("system"), System);
		double MaxTokensNum = 2048.0;
		Args->TryGetNumberField(TEXT("maxTokens"), MaxTokensNum);
		const FString Model = ResolveModel(Args);

		FString Out, Err;
		if (!CallAnthropic(System, Prompt, Model, FMath::Clamp(static_cast<int32>(MaxTokensNum), 64, 8192), Out, Err))
		{
			return ErrorJson(Err);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("model"), Model);
		Result->SetStringField(TEXT("text"), Out);
		return SuccessJson(Result);
	}

	// One-shot "PCG copilot": pull the local PCG knowledge base + optional web context, then have
	// an LLM synthesize a concrete node-by-node plan. Combines knowledge library + network + LLM.
	FString PcgAssist(const TSharedPtr<FJsonObject>& Args)
	{
		FString Task;
		if (!Args->TryGetStringField(TEXT("task"), Task) || Task.IsEmpty())
		{
			return ErrorJson(TEXT("Missing 'task'."));
		}
		bool bIncludeWeb = true;
		Args->TryGetBoolField(TEXT("includeWeb"), bIncludeWeb);

		// 1) PCG knowledge base (reuse the existing PcgKnowledgeTools dispatch).
		FString Knowledge;
		auto CallPcg = [&Knowledge, &Task](const TCHAR* Tool, int32 TopK)
		{
			TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetStringField(TEXT("query"), Task);
			A->SetNumberField(TEXT("top_k"), TopK);
			FString R;
			if (WorldDataMCP::PcgKnowledgeTools::Dispatch(Tool, A, R))
			{
				Knowledge += FString::Printf(TEXT("\n## %s\n%s\n"), Tool, *R);
			}
		};
		CallPcg(TEXT("pcg_search"), 8);
		CallPcg(TEXT("pcg_get_workflow"), 2);
		CallPcg(TEXT("pcg_find_gotchas"), 5);

		// 2) Optional web context.
		FString Web;
		bool bWebUsed = false;
		if (bIncludeWeb)
		{
			TSharedRef<FJsonObject> WebArgs = MakeShared<FJsonObject>();
			WebArgs->SetStringField(TEXT("query"), Task + TEXT(" PCG"));
			const FString WebResult = WebSearch(WebArgs);
			const TSharedPtr<FJsonObject> WebJson = ParseJsonObject(WebResult);
			if (WebJson.IsValid() && WebJson->HasField(TEXT("results")))
			{
				Web = WebResult;
				bWebUsed = true;
			}
		}

		// 3) LLM synthesis.
		const FString System = TEXT("You are an expert Unreal Engine 5 PCG (Procedural Content Generation) technical artist. Using the provided PCG knowledge-base excerpts and web context (plus well-established UE5 facts), produce a concrete, correct, step-by-step plan for the task: which PCG nodes to add, how to wire them (output->input), key parameter values, and pitfalls to avoid. Prefer cross-verified nodes. If the knowledge base lacks something, say so explicitly rather than inventing node names. Be concise and actionable.");
		const FString User = FString::Printf(TEXT("TASK:\n%s\n\n=== PCG KNOWLEDGE BASE ===\n%s\n\n=== WEB CONTEXT ===\n%s"),
			*Task, *Knowledge, *Web);
		double MaxTokensNum = 2048.0;
		Args->TryGetNumberField(TEXT("maxTokens"), MaxTokensNum);
		const FString Model = ResolveModel(Args);

		FString Out, Err;
		if (!CallAnthropic(System, User, Model, FMath::Clamp(static_cast<int32>(MaxTokensNum), 256, 8192), Out, Err))
		{
			return ErrorJson(Err);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("task"), Task);
		Result->SetStringField(TEXT("plan"), Out);
		Result->SetBoolField(TEXT("usedKnowledgeBase"), !Knowledge.IsEmpty());
		Result->SetBoolField(TEXT("usedWeb"), bWebUsed);
		Result->SetStringField(TEXT("model"), Model);
		Result->SetStringField(TEXT("hint"), TEXT("Execute the plan with the PCG graph tools: create_pcg_graph / add_pcg_node / connect_pcg_nodes / set_pcg_node_settings / execute_pcg_graph."));
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"ue_web_search","description":"Look up how to do something in Unreal Engine at runtime (the correct approach, the right levers, common gotchas) before acting. Queries are biased toward Unreal Engine unless raw=true. Requires env var UEMCP_SEARCH_API_KEY (provider via UEMCP_SEARCH_PROVIDER=tavily|brave, default tavily). Follow up with ue_fetch_doc to read a result in full.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"maxResults":{"type":"number","description":"Default 5, capped 15."},"raw":{"type":"boolean","description":"Skip the 'Unreal Engine' query bias."}},"required":["query"]},"annotations":{"title":"UE Web Search","readOnlyHint":true,"openWorldHint":true}},
{"name":"ue_fetch_doc","description":"Fetch a web page (e.g. an Unreal Engine documentation URL) and return its text content. No API key needed. Use after ue_web_search to read the authoritative approach before changing the scene.","inputSchema":{"type":"object","properties":{"url":{"type":"string"},"maxBytes":{"type":"number","description":"Max returned characters. Default 20000."}},"required":["url"]},"annotations":{"title":"UE Fetch Doc","readOnlyHint":true,"openWorldHint":true}},
{"name":"describe_scene","description":"High-level spatial + lighting digest of the current world — read this FIRST to understand WHAT the scene is. Returns: overall world bounds, a semantic category histogram (foliage/landscape/spline/light/staticMesh/...), dense regions (clusters with a dominant category, centre, size and sample actors — the level at an altitude you can reason about), the largest landmarks, and the lighting drivers (sun angle/intensity, SkyAtmosphere/fog/SkyLight, post-process count). For a picture of the layout call capture_scene_map; to drill into an area use query_actors_in_region / find_nearest_actors.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Describe Scene","readOnlyHint":true,"openWorldHint":false}},
{"name":"llm_think","description":"Server-side LLM reasoning: send a prompt (and optional system instruction) to the Anthropic API and return the model's text. Requires env var ANTHROPIC_API_KEY (model via UEMCP_LLM_MODEL, default claude-sonnet-4-6). Blocks the editor until the call returns.","inputSchema":{"type":"object","properties":{"prompt":{"type":"string"},"system":{"type":"string"},"model":{"type":"string"},"maxTokens":{"type":"number","description":"Default 2048."}},"required":["prompt"]},"annotations":{"title":"LLM Think","readOnlyHint":true,"openWorldHint":true}},
{"name":"pcg_assist","description":"PCG copilot: for a high-level task, automatically pulls the local PCG knowledge base (search + workflow + gotchas), optionally web context, then has an LLM synthesize a concrete node-by-node PCG plan. Requires ANTHROPIC_API_KEY (and UEMCP_SEARCH_API_KEY for web). Returns a 'plan' to execute with the PCG graph tools.","inputSchema":{"type":"object","properties":{"task":{"type":"string","description":"e.g. 'scatter small rocks around big rocks on a landscape'."},"includeWeb":{"type":"boolean","description":"Also search the web for context. Default true."},"model":{"type":"string"},"maxTokens":{"type":"number","description":"Default 2048."}},"required":["task"]},"annotations":{"title":"PCG Assist","readOnlyHint":true,"openWorldHint":true}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("ue_web_search")) { OutResult = WebSearch(Args); return true; }
	if (ToolName == TEXT("ue_fetch_doc")) { OutResult = FetchDoc(Args); return true; }
	if (ToolName == TEXT("describe_scene")) { OutResult = DescribeScene(Args); return true; }
	if (ToolName == TEXT("llm_think")) { OutResult = LlmThink(Args); return true; }
	if (ToolName == TEXT("pcg_assist")) { OutResult = PcgAssist(Args); return true; }
	return false;
}
}
}
