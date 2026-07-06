#include "WorldDataMCPPcgKnowledgeTools.h"

#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace WorldDataMCP
{
namespace PcgKnowledgeTools
{
namespace
{
	// ----------------------------------------------------------------- index
	struct FPcgIndex
	{
		bool bLoaded = false;
		bool bOk = false;
		FString LoadError;

		TSharedPtr<FJsonObject> Root;
		TArray<TSharedPtr<FJsonObject>> Nodes;
		TArray<TSharedPtr<FJsonObject>> Gotchas;
		TArray<TSharedPtr<FJsonObject>> Patterns;
		TArray<TSharedPtr<FJsonObject>> Workflows;
		TArray<TSharedPtr<FJsonObject>> Docs;

		TMap<FString, int32> NodeByKey;            // normalized name/alias -> Nodes index

		// BM25 over Docs (title + text)
		TArray<TMap<FString, int32>> DocTf;        // per-doc term frequencies
		TArray<int32> DocLen;
		TMap<FString, int32> DocFreq;              // term -> #docs containing it
		double AvgDocLen = 1.0;
	};

	FString NormKey(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (TCHAR C : In)
		{
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	void Tokenize(const FString& Text, TArray<FString>& Out)
	{
		FString Cur;
		auto Flush = [&]()
		{
			if (Cur.Len() >= 2) { Out.Add(Cur); }
			Cur.Reset();
		};
		for (TCHAR C : Text)
		{
			if (FChar::IsAlnum(C)) { Cur.AppendChar(FChar::ToLower(C)); }
			else { Flush(); }
		}
		Flush();
	}

	const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid()) { Obj->TryGetArrayField(Field, Arr); }
		return Arr;
	}

	FString DataFilePath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP"));
		if (Plugin.IsValid())
		{
			return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Data"), TEXT("pcg_knowledge.json"));
		}
		return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UEBridgeMCP"), TEXT("Data"), TEXT("pcg_knowledge.json"));
	}

	FPcgIndex& GetIndex()
	{
		static FPcgIndex Index;
		if (Index.bLoaded)
		{
			return Index;
		}
		Index.bLoaded = true;

		const FString Path = DataFilePath();
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path))
		{
			Index.LoadError = FString::Printf(TEXT("pcg_knowledge.json not found at %s"), *Path);
			return Index;
		}
		Index.Root = WorldDataMCP::ParseJsonObject(Raw);
		if (!Index.Root.IsValid())
		{
			Index.LoadError = TEXT("pcg_knowledge.json failed to parse");
			return Index;
		}

		auto Collect = [&](const TCHAR* Field, TArray<TSharedPtr<FJsonObject>>& Dst)
		{
			if (const TArray<TSharedPtr<FJsonValue>>* Arr = GetArray(Index.Root, Field))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					if (V.IsValid() && V->Type == EJson::Object) { Dst.Add(V->AsObject()); }
				}
			}
		};
		Collect(TEXT("nodes"), Index.Nodes);
		Collect(TEXT("gotchas"), Index.Gotchas);
		Collect(TEXT("patterns"), Index.Patterns);
		Collect(TEXT("workflows"), Index.Workflows);
		Collect(TEXT("docs"), Index.Docs);

		// node lookup by name + aliases
		for (int32 i = 0; i < Index.Nodes.Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& N = Index.Nodes[i];
			FString Name;
			if (N->TryGetStringField(TEXT("name"), Name))
			{
				Index.NodeByKey.Add(NormKey(Name), i);
			}
			if (const TArray<TSharedPtr<FJsonValue>>* Aliases = GetArray(N, TEXT("aliases")))
			{
				for (const TSharedPtr<FJsonValue>& A : *Aliases)
				{
					const FString K = NormKey(A->AsString());
					if (!K.IsEmpty() && !Index.NodeByKey.Contains(K)) { Index.NodeByKey.Add(K, i); }
				}
			}
		}

		// BM25 over docs
		int64 TotalLen = 0;
		Index.DocTf.Reserve(Index.Docs.Num());
		Index.DocLen.Reserve(Index.Docs.Num());
		for (const TSharedPtr<FJsonObject>& D : Index.Docs)
		{
			FString Title, Text;
			D->TryGetStringField(TEXT("title"), Title);
			D->TryGetStringField(TEXT("text"), Text);
			TArray<FString> Toks;
			Tokenize(Title + TEXT(" ") + Text, Toks);
			TMap<FString, int32> Tf;
			for (const FString& T : Toks) { Tf.FindOrAdd(T)++; }
			for (const TPair<FString, int32>& P : Tf) { Index.DocFreq.FindOrAdd(P.Key)++; }
			TotalLen += Toks.Num();
			Index.DocLen.Add(Toks.Num());
			Index.DocTf.Add(MoveTemp(Tf));
		}
		Index.AvgDocLen = Index.Docs.Num() > 0 ? double(TotalLen) / Index.Docs.Num() : 1.0;
		Index.bOk = true;
		return Index;
	}

	// ----------------------------------------------------------------- helpers
	TSharedRef<FJsonObject> CloneFields(const TSharedPtr<FJsonObject>& Src)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		if (Src.IsValid()) { R->Values = Src->Values; }
		return R;
	}

	FString Snippet(const FString& Text, int32 Max = 240)
	{
		FString S = Text.Replace(TEXT("\n"), TEXT(" "));
		return S.Len() > Max ? (S.Left(Max) + TEXT("...")) : S;
	}

	int32 TokenOverlap(const TArray<FString>& Q, const FString& Text)
	{
		TArray<FString> T; Tokenize(Text, T);
		TSet<FString> S(T);
		int32 Hits = 0;
		for (const FString& Tok : Q) { if (S.Contains(Tok)) { ++Hits; } }
		return Hits;
	}

	// ----------------------------------------------------------------- tools
	FString SearchPcg(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }

		FString Query;
		if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
		{
			return ErrorJson(TEXT("'query' is required"));
		}
		int32 TopK = 8; Args->TryGetNumberField(TEXT("top_k"), TopK);
		TopK = FMath::Clamp(TopK, 1, 30);
		FString TypeFilter; Args->TryGetStringField(TEXT("type"), TypeFilter);

		TArray<FString> QToks; Tokenize(Query, QToks);
		const int32 N = Idx.Docs.Num();
		const double k1 = 1.5, b = 0.75;

		TArray<TPair<int32, double>> Scored;
		for (int32 i = 0; i < N; ++i)
		{
			if (!TypeFilter.IsEmpty())
			{
				FString Type; Idx.Docs[i]->TryGetStringField(TEXT("type"), Type);
				if (!Type.Equals(TypeFilter, ESearchCase::IgnoreCase)) { continue; }
			}
			double Score = 0.0;
			const TMap<FString, int32>& Tf = Idx.DocTf[i];
			const double DL = FMath::Max(1, Idx.DocLen[i]);
			for (const FString& T : QToks)
			{
				const int32* Freq = Tf.Find(T);
				if (!Freq) { continue; }
				const int32 Df = Idx.DocFreq.FindRef(T);
				const double Idf = FMath::Loge(1.0 + (double(N) - Df + 0.5) / (Df + 0.5));
				Score += Idf * (*Freq * (k1 + 1.0)) / (*Freq + k1 * (1.0 - b + b * DL / Idx.AvgDocLen));
			}
			if (Score > 0.0) { Scored.Add({ i, Score }); }
		}
		Scored.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });

		TArray<TSharedPtr<FJsonValue>> Results;
		for (int32 r = 0; r < Scored.Num() && r < TopK; ++r)
		{
			const TSharedPtr<FJsonObject>& D = Idx.Docs[Scored[r].Key];
			TSharedRef<FJsonObject> Hit = MakeShared<FJsonObject>();
			FString S;
			D->TryGetStringField(TEXT("id"), S); Hit->SetStringField(TEXT("id"), S);
			D->TryGetStringField(TEXT("type"), S); Hit->SetStringField(TEXT("type"), S);
			D->TryGetStringField(TEXT("title"), S); Hit->SetStringField(TEXT("title"), S);
			D->TryGetStringField(TEXT("node"), S); if (!S.IsEmpty()) { Hit->SetStringField(TEXT("node"), S); }
			D->TryGetStringField(TEXT("version"), S); if (!S.IsEmpty()) { Hit->SetStringField(TEXT("version"), S); }
			FString Text; D->TryGetStringField(TEXT("text"), Text);
			Hit->SetStringField(TEXT("snippet"), Snippet(Text));
			Hit->SetNumberField(TEXT("score"), FMath::RoundToDouble(Scored[r].Value * 1000.0) / 1000.0);
			Results.Add(MakeShared<FJsonValueObject>(Hit));
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("count"), Results.Num());
		Out->SetArrayField(TEXT("results"), Results);
		return SuccessJson(Out);
	}

	FString GetNode(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return ErrorJson(TEXT("'name' is required"));
		}
		const int32* I = Idx.NodeByKey.Find(NormKey(Name));
		if (!I)
		{
			// suggest near matches
			TArray<TSharedPtr<FJsonValue>> Sugg;
			const FString K = NormKey(Name);
			for (const TSharedPtr<FJsonObject>& N : Idx.Nodes)
			{
				FString NN; N->TryGetStringField(TEXT("name"), NN);
				if (NormKey(NN).Contains(K) || K.Contains(NormKey(NN)))
				{
					Sugg.Add(MakeShared<FJsonValueString>(NN));
					if (Sugg.Num() >= 6) { break; }
				}
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("found"), false);
			Out->SetStringField(TEXT("query"), Name);
			Out->SetArrayField(TEXT("suggestions"), Sugg);
			return SuccessJson(Out);
		}
		TSharedRef<FJsonObject> Out = CloneFields(Idx.Nodes[*I]);
		Out->SetBoolField(TEXT("found"), true);
		return SuccessJson(Out);
	}

	FString LookupParameter(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Node, Param;
		Args->TryGetStringField(TEXT("node"), Node);
		Args->TryGetStringField(TEXT("parameter"), Param);
		if (Node.IsEmpty() || Param.IsEmpty()) { return ErrorJson(TEXT("'node' and 'parameter' are required")); }
		const int32* I = Idx.NodeByKey.Find(NormKey(Node));
		if (!I) { return ErrorJson(FString::Printf(TEXT("node not found: %s"), *Node)); }
		const FString PK = NormKey(Param);
		if (const TArray<TSharedPtr<FJsonValue>>* Params = GetArray(Idx.Nodes[*I], TEXT("params")))
		{
			for (const TSharedPtr<FJsonValue>& V : *Params)
			{
				const TSharedPtr<FJsonObject> P = V->AsObject();
				if (!P.IsValid()) { continue; }
				FString PN; P->TryGetStringField(TEXT("parameter"), PN);
				if (NormKey(PN).Contains(PK) || PK.Contains(NormKey(PN)))
				{
					TSharedRef<FJsonObject> Out = CloneFields(P);
					Out->SetBoolField(TEXT("found"), true);
					FString NN; Idx.Nodes[*I]->TryGetStringField(TEXT("name"), NN);
					Out->SetStringField(TEXT("node"), NN);
					return SuccessJson(Out);
				}
			}
		}
		return ErrorJson(FString::Printf(TEXT("parameter '%s' not found on node '%s'"), *Param, *Node));
	}

	// shared ranked-match over an array of {fields...} by token overlap with a query
	FString RankMatch(const TArray<TSharedPtr<FJsonObject>>& Items, const TArray<FString>& MatchFields,
		const FString& Query, int32 TopK, const TCHAR* ResultKey)
	{
		TArray<FString> QToks; Tokenize(Query, QToks);
		TArray<TPair<int32, int32>> Scored;
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			FString Blob;
			for (const FString& F : MatchFields)
			{
				FString S; Items[i]->TryGetStringField(F, S); Blob += S + TEXT(" ");
			}
			const int32 Hits = TokenOverlap(QToks, Blob);
			if (Hits > 0) { Scored.Add({ i, Hits }); }
		}
		Scored.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Value > B.Value; });
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 r = 0; r < Scored.Num() && r < TopK; ++r)
		{
			Out.Add(MakeShared<FJsonValueObject>(CloneFields(Items[Scored[r].Key])));
		}
		TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("count"), Out.Num());
		Res->SetArrayField(ResultKey, Out);
		return SuccessJson(Res);
	}

	FString FindGotchas(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Query; Args->TryGetStringField(TEXT("query"), Query);
		if (Query.IsEmpty()) { return ErrorJson(TEXT("'query' is required (node name or topic)")); }
		int32 TopK = 8; Args->TryGetNumberField(TEXT("top_k"), TopK); TopK = FMath::Clamp(TopK, 1, 25);
		return RankMatch(Idx.Gotchas, { TEXT("problem"), TEXT("fix") }, Query, TopK, TEXT("gotchas"));
	}

	FString FindPatterns(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Query; Args->TryGetStringField(TEXT("query"), Query);
		if (Query.IsEmpty()) { return ErrorJson(TEXT("'query' is required (goal description)")); }
		int32 TopK = 8; Args->TryGetNumberField(TEXT("top_k"), TopK); TopK = FMath::Clamp(TopK, 1, 25);
		return RankMatch(Idx.Patterns, { TEXT("name"), TEXT("description") }, Query, TopK, TEXT("patterns"));
	}

	FString GetWorkflow(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Query; Args->TryGetStringField(TEXT("query"), Query);
		if (Query.IsEmpty()) { return ErrorJson(TEXT("'query' is required (effect or video topic)")); }
		int32 TopK = 3; Args->TryGetNumberField(TEXT("top_k"), TopK); TopK = FMath::Clamp(TopK, 1, 8);
		return RankMatch(Idx.Workflows, { TEXT("title") }, Query, TopK, TEXT("workflows"));
	}

	FString ListNodes(const TSharedPtr<FJsonObject>& Args)
	{
		FPcgIndex& Idx = GetIndex();
		if (!Idx.bOk) { return ErrorJson(Idx.LoadError); }
		FString Version; Args->TryGetStringField(TEXT("version"), Version);
		int32 MinSources = 1; Args->TryGetNumberField(TEXT("min_sources"), MinSources);
		int32 Limit = 60; Args->TryGetNumberField(TEXT("limit"), Limit); Limit = FMath::Clamp(Limit, 1, 600);

		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSharedPtr<FJsonObject>& N : Idx.Nodes)
		{
			int32 SC = 0; N->TryGetNumberField(TEXT("source_count"), SC);
			if (SC < MinSources) { continue; }
			if (!Version.IsEmpty())
			{
				bool bMatch = false;
				if (const TArray<TSharedPtr<FJsonValue>>* Vers = GetArray(N, TEXT("versions")))
				{
					for (const TSharedPtr<FJsonValue>& V : *Vers)
					{
						if (V->AsString().Contains(Version)) { bMatch = true; break; }
					}
				}
				if (!bMatch) { continue; }
			}
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			FString S;
			N->TryGetStringField(TEXT("name"), S); Row->SetStringField(TEXT("name"), S);
			N->TryGetStringField(TEXT("trust"), S); Row->SetStringField(TEXT("trust"), S);
			Row->SetNumberField(TEXT("source_count"), SC);
			if (const TArray<TSharedPtr<FJsonValue>>* P = GetArray(N, TEXT("params")))
			{
				Row->SetNumberField(TEXT("param_count"), P->Num());
			}
			Out.Add(MakeShared<FJsonValueObject>(Row));
			if (Out.Num() >= Limit) { break; }
		}
		TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("count"), Out.Num());
		Res->SetArrayField(TEXT("nodes"), Out);
		return SuccessJson(Res);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"pcg_search","description":"Full-text (BM25) search over a distilled UE5 PCG knowledge corpus (node cards, gotchas, reusable patterns, workflows). Returns ranked snippets with type, source node, and UE version. Use for open questions like 'scatter rocks by density' or 'spline road generation'.","inputSchema":{"type":"object","properties":{"query":{"type":"string","description":"Natural-language or keyword query (English node/param names work best)."},"top_k":{"type":"integer","description":"Max results (1-30, default 8)."},"type":{"type":"string","description":"Optional filter: node|gotcha|pattern|workflow|overview."}},"required":["query"]},"annotations":{"title":"PCG Search","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_get_node","description":"Look up one PCG node by name or alias (e.g. 'Copy Points', 'CopyPoints', 'Density Filter'). Returns purpose, parameters with effects + observed values, UE versions, trust (cross-verified vs single-source), and an evidence quote with timestamp. If not found, returns suggestions.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"PCG node name or alias."}},"required":["name"]},"annotations":{"title":"PCG Get Node","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_lookup_parameter","description":"Get the effect and observed values of a specific parameter on a PCG node (e.g. node 'Surface Sampler', parameter 'Looseness').","inputSchema":{"type":"object","properties":{"node":{"type":"string"},"parameter":{"type":"string"}},"required":["node","parameter"]},"annotations":{"title":"PCG Lookup Parameter","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_find_gotchas","description":"Find known PCG pitfalls and their fixes matching a node name or topic (e.g. 'copy points index', 'plugin not enabled', 'spline density').","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"top_k":{"type":"integer","description":"Max results (1-25, default 8)."}},"required":["query"]},"annotations":{"title":"PCG Find Gotchas","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_find_patterns","description":"Find reusable PCG graph patterns/recipes matching a goal (e.g. 'scatter small rocks around big rocks', 'distance based density falloff').","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"top_k":{"type":"integer","description":"Max results (1-25, default 8)."}},"required":["query"]},"annotations":{"title":"PCG Find Patterns","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_get_workflow","description":"Return the best-matching end-to-end PCG workflow (ordered steps with timestamps) for an effect or tutorial topic, e.g. 'jungle scatter', 'spline road'.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"top_k":{"type":"integer","description":"Max workflows (1-8, default 3)."}},"required":["query"]},"annotations":{"title":"PCG Get Workflow","readOnlyHint":true,"openWorldHint":false}},
{"name":"pcg_list_nodes","description":"List known PCG nodes, optionally filtered by UE version (e.g. '5.4') and minimum number of corroborating sources. Returns name, trust, source_count, param_count.","inputSchema":{"type":"object","properties":{"version":{"type":"string","description":"UE version substring filter, e.g. '5.4'."},"min_sources":{"type":"integer","description":"Only nodes seen in at least this many tutorials (default 1; use 2+ for cross-verified)."},"limit":{"type":"integer","description":"Max rows (default 60)."}}},"annotations":{"title":"PCG List Nodes","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("pcg_search")) { OutResult = SearchPcg(Args); return true; }
	if (ToolName == TEXT("pcg_get_node")) { OutResult = GetNode(Args); return true; }
	if (ToolName == TEXT("pcg_lookup_parameter")) { OutResult = LookupParameter(Args); return true; }
	if (ToolName == TEXT("pcg_find_gotchas")) { OutResult = FindGotchas(Args); return true; }
	if (ToolName == TEXT("pcg_find_patterns")) { OutResult = FindPatterns(Args); return true; }
	if (ToolName == TEXT("pcg_get_workflow")) { OutResult = GetWorkflow(Args); return true; }
	if (ToolName == TEXT("pcg_list_nodes")) { OutResult = ListNodes(Args); return true; }
	return false;
}
}
}
