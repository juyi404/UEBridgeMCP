#include "WorldDataSceneBriefStore.h"

#include "HAL/PlatformTime.h"

namespace WorldDataMCP
{
	// Single active brief. MCP tool dispatch runs on the game thread (the server marshals
	// calls), so a plain file-static needs no locking.
	static FSceneBrief GActiveSceneBrief;

	// Verification lifecycle for the active scene — same single-active, game-thread, lock-free model.
	static FSceneVerifyState GVerifyState;

	bool SetSceneBrief(const FSceneBrief& In, FString& OutError)
	{
		const FString Lang = In.Language.TrimStartAndEnd().ToLower();
		if (Lang != TEXT("formal") && Lang != TEXT("informal"))
		{
			OutError = TEXT("language must be exactly \"formal\" or \"informal\" — pick ONE compositional language, never both (design-playbook §1).");
			return false;
		}

		TArray<FString> Missing;
		if (In.OneIdea.TrimStartAndEnd().IsEmpty())    { Missing.Add(TEXT("oneIdea")); }
		if (In.Hero.TrimStartAndEnd().IsEmpty())       { Missing.Add(TEXT("hero")); }
		if (In.Palette.TrimStartAndEnd().IsEmpty())    { Missing.Add(TEXT("palette")); }
		if (In.Atmosphere.TrimStartAndEnd().IsEmpty()) { Missing.Add(TEXT("atmosphere")); }
		if (In.Boundary.TrimStartAndEnd().IsEmpty())   { Missing.Add(TEXT("boundary")); }
		if (Missing.Num() > 0)
		{
			OutError = FString::Printf(
				TEXT("scene brief missing required field(s): %s. A brief needs all of: oneIdea, language(formal|informal), hero, palette, atmosphere, boundary."),
				*FString::Join(Missing, TEXT(", ")));
			return false;
		}

		FSceneBrief B = In;
		B.Language = Lang;
		B.SetTimeSeconds = FPlatformTime::Seconds();
		B.bValid = true;
		GActiveSceneBrief = B;

		// A fresh concept brief starts a new authoring cycle: nothing has been generated under it
		// yet, so the scene is clean (not dirty, no stale verdict) until the first generation runs.
		GVerifyState = FSceneVerifyState();
		GVerifyState.LevelName = B.LevelName;

		OutError.Reset();
		return true;
	}

	bool HasActiveSceneBrief(const FString& CurrentLevelName, FString& OutReason)
	{
		if (!GActiveSceneBrief.bValid)
		{
			OutReason = TEXT("BLOCKED: no active scene brief. Scene generation is gated — call set_scene_brief first with {oneIdea, language:\"formal\"|\"informal\", hero, palette, atmosphere, boundary}, then generate. (This enforces the design-playbook CONCEPT BRIEF as a hard precondition.)");
			return false;
		}
		if (!CurrentLevelName.IsEmpty() && !GActiveSceneBrief.LevelName.IsEmpty()
			&& !GActiveSceneBrief.LevelName.Equals(CurrentLevelName, ESearchCase::IgnoreCase))
		{
			OutReason = FString::Printf(
				TEXT("BLOCKED: the active scene brief was set for level '%s' but the current level is '%s'. Call set_scene_brief again for this level before generating."),
				*GActiveSceneBrief.LevelName, *CurrentLevelName);
			return false;
		}
		OutReason.Reset();
		return true;
	}

	FSceneBrief GetActiveSceneBrief()
	{
		return GActiveSceneBrief;
	}

	TArray<FString> MapPaletteKeywordsToMeshes(const FString& Palette)
	{
		const FString Lower = Palette.ToLower();
		TArray<FString> Meshes;
		auto AddUnique = [&Meshes](const TCHAR* Path)
		{
			if (!Meshes.Contains(Path))
			{
				Meshes.Add(Path);
			}
		};

		if (Lower.Contains(TEXT("tree")) || Lower.Contains(TEXT("pine")) || Lower.Contains(TEXT("conifer"))
			|| Lower.Contains(TEXT("canopy")) || Lower.Contains(TEXT("oak")) || Lower.Contains(TEXT("cedar")))
		{
			AddUnique(TEXT("/Engine/BasicShapes/Cone.Cone"));
		}
		if (Lower.Contains(TEXT("rock")) || Lower.Contains(TEXT("stone")) || Lower.Contains(TEXT("boulder")))
		{
			AddUnique(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		}
		if (Lower.Contains(TEXT("shrub")) || Lower.Contains(TEXT("bush")) || Lower.Contains(TEXT("hedge"))
			|| Lower.Contains(TEXT("understory")))
		{
			AddUnique(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		}
		if (Lower.Contains(TEXT("grass")) || Lower.Contains(TEXT("groundcover")) || Lower.Contains(TEXT("moss"))
			|| Lower.Contains(TEXT("fern")) || Lower.Contains(TEXT("boxwood")) || Lower.Contains(TEXT("topiary")))
		{
			AddUnique(TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		if (Meshes.Num() == 0)
		{
			AddUnique(TEXT("/Engine/BasicShapes/Cone.Cone"));
			AddUnique(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		}
		return Meshes;
	}

	FSceneBriefRouting BuildBriefRouting(const FSceneBrief& Brief)
	{
		FSceneBriefRouting R;
		if (!Brief.bValid)
		{
			R.Guidance = TEXT("No active brief — call set_scene_brief before generating.");
			return R;
		}

		R.Language = Brief.Language;
		R.bFormal = Brief.Language == TEXT("formal");
		R.PaletteMeshes = MapPaletteKeywordsToMeshes(Brief.Palette);

		const FString IdeaLower = (Brief.OneIdea + TEXT(" ") + Brief.Boundary).ToLower();
		const bool bPathScene = IdeaLower.Contains(TEXT("path")) || IdeaLower.Contains(TEXT("trail"))
			|| IdeaLower.Contains(TEXT("forest")) || IdeaLower.Contains(TEXT("walk"));

		if (R.bFormal)
		{
			R.RecommendedTool = TEXT("place_along_axis");
			R.RecommendedRuleId.Reset();
			R.Guidance = TEXT("formal brief: prefer axis/grid placement (place_along_axis) for allées and hedges; avoid random scatter. If PCG is needed, use planting_stack with zero Z rotation and lower density.");
		}
		else
		{
			R.RecommendedTool = TEXT("apply_native_pcg_rule");
			R.RecommendedRuleId = bPathScene ? TEXT("forest_path") : TEXT("packing_scatter");
			if (IdeaLower.Contains(TEXT("layer")) || IdeaLower.Contains(TEXT("understory"))
				|| IdeaLower.Contains(TEXT("canopy")) || IdeaLower.Contains(TEXT("garden")))
			{
				R.RecommendedRuleId = TEXT("planting_stack");
			}
			R.Guidance = FString::Printf(
				TEXT("informal brief: use apply_native_pcg_rule with ruleId '%s' (or planting_stack for layered vegetation). Palette meshes mapped from brief."),
				*R.RecommendedRuleId);
		}
		return R;
	}

	void ClearSceneBrief()
	{
		GActiveSceneBrief = FSceneBrief();
		GVerifyState = FSceneVerifyState();
	}

	void MarkSceneDirty(const FString& CurrentLevelName)
	{
		// A level switch invalidates the prior scene's verify state — start its lifecycle fresh.
		if (!GVerifyState.LevelName.Equals(CurrentLevelName, ESearchCase::IgnoreCase))
		{
			GVerifyState = FSceneVerifyState();
			GVerifyState.LevelName = CurrentLevelName;
		}
		GVerifyState.bDirty = true;
		GVerifyState.GeneratedTimeSeconds = FPlatformTime::Seconds();
	}

	void RecordGeometricVerdict(const FString& CurrentLevelName, bool bPass, int32 IssueCount, const FString& Summary)
	{
		GVerifyState.LevelName = CurrentLevelName;
		GVerifyState.bDirty = false;               // a whole-scene verdict resolves the dirty state
		GVerifyState.bEverVerified = true;
		GVerifyState.bLastVerdictPass = bPass;
		GVerifyState.LastIssueCount = IssueCount;
		GVerifyState.LastSummary = Summary;
		GVerifyState.VerifiedTimeSeconds = FPlatformTime::Seconds();
	}

	bool HasUnresolvedSceneVerdict(const FString& CurrentLevelName, FString& OutReason)
	{
		// State for a different level says nothing about this one — nothing to block.
		if (!CurrentLevelName.IsEmpty() && !GVerifyState.LevelName.IsEmpty()
			&& !GVerifyState.LevelName.Equals(CurrentLevelName, ESearchCase::IgnoreCase))
		{
			OutReason.Reset();
			return false;
		}

		if (GVerifyState.bDirty)
		{
			OutReason = TEXT("BLOCKED: this scene has unverified generated changes. Run verify_scene (whole-scene geometric review) before saving. If you must persist work-in-progress anyway, pass confirmUnverified:true to acknowledge the scene is unverified.");
			return true;
		}
		if (GVerifyState.bEverVerified && !GVerifyState.bLastVerdictPass)
		{
			OutReason = FString::Printf(
				TEXT("BLOCKED: the last geometric verdict for this scene was FAIL (%d issue(s): %s). Refine and re-run verify_scene until it passes. To save the known-broken scene anyway, pass confirmUnverified:true."),
				GVerifyState.LastIssueCount, *GVerifyState.LastSummary);
			return true;
		}

		OutReason.Reset();
		return false;
	}

	FSceneVerifyState GetSceneVerifyState()
	{
		return GVerifyState;
	}
}
