#pragma once

#include "CoreMinimal.h"

// Scene-generation gate ("concept-first" enforced as a mechanism, not just skill advice).
//
// Before ANY content-generation MCP tool runs (place_meshes / lay_meshes_along_spline /
// add_foliage_instances / spawn_actor-with-mesh / apply_native_pcg_rule), a design brief
// MUST exist. The tools call HasActiveSceneBrief() at their entry and refuse otherwise.
//
// This lives in UEBridgeMCPCore because BOTH the UEBridgeMCP editor tools and the
// NativePCGRuleBridge MCP tool already depend on this module, so both gate against the
// SAME state. State is editor-time, single active brief, set via the set_scene_brief MCP
// tool, and authorises generation only for the level it was set in (a brief for level A
// must not silently authorise generation in level B).
namespace WorldDataMCP
{
	struct FSceneBrief
	{
		FString OneIdea;     // §0 the single sentence ("a quiet formal memorial garden at golden hour")
		FString Language;    // §1 "formal" | "informal" — exactly one compositional language
		FString Hero;        // §2 focal point + how it is framed
		FString Palette;     // §9 2-3 materials/species, one dominant
		FString Atmosphere;  // §7 time of day + sun angle + grade target
		FString Boundary;    // §6 how the space is contained + where the entry/threshold is
		FString LevelName;   // the level this brief authorises generation in
		double  SetTimeSeconds = 0.0;
		bool    bValid = false;
	};

	// Store/replace the active brief. Returns false + OutError if any required field is
	// empty or Language is not exactly "formal"/"informal". On success, stamps LevelName
	// and time. Caller supplies the current level name (Core has no Engine dependency).
	UEBRIDGEMCPCORE_API bool SetSceneBrief(const FSceneBrief& In, FString& OutError);

	// True if a valid brief exists for CurrentLevelName. When false, OutReason explains
	// why (none set, or set for a different level) and how to fix it — feed it straight
	// back to the model as the tool's refusal message.
	UEBRIDGEMCPCORE_API bool HasActiveSceneBrief(const FString& CurrentLevelName, FString& OutReason);

	// Copy of the active brief (bValid == false if none).
	UEBRIDGEMCPCORE_API FSceneBrief GetActiveSceneBrief();

	// Brief-driven generation routing: maps language/palette/oneIdea into recommended tools,
	// PCG rules, and palette-derived mesh paths. Consumed by apply_native_pcg_rule, bootstrap
	// sceneBuilding hints, and formal layout tools — brief is no longer gate-only.
	struct FSceneBriefRouting
	{
		FString Language;
		FString RecommendedTool;   // place_along_axis | apply_native_pcg_rule | place_meshes
		FString RecommendedRuleId; // forest_path | packing_scatter | planting_stack | (empty when formal axis layout)
		FString Guidance;
		TArray<FString> PaletteMeshes;
		bool bFormal = false;
	};

	UEBRIDGEMCPCORE_API FSceneBriefRouting BuildBriefRouting(const FSceneBrief& Brief);

	// Keyword scan of palette text -> StaticMesh asset paths (BasicShapes placeholders).
	UEBRIDGEMCPCORE_API TArray<FString> MapPaletteKeywordsToMeshes(const FString& Palette);

	// Drop the active brief (e.g. on level reset / when starting a different scene).
	UEBRIDGEMCPCORE_API void ClearSceneBrief();

	// ---------------------------------------------------------------------------------------------
	// Verification gate (the post-condition MIRROR of the scene-brief pre-condition gate).
	//
	// scene-brief proved that a SERVER-SIDE gate is the only constraint that survives contact with a
	// shortcut-seeking agent — a skill/prompt instruction the executing agent cannot reach does
	// nothing. So verification is enforced here too, not in a skill: every generation marks the scene
	// "needs verification"; only a whole-scene verify_scene clears it; "commit" chokepoints (save)
	// refuse while the scene is dirty or its last geometric verdict was a FAIL.
	//
	// This struct is PURE DATA (Core has no Engine dependency). The deterministic geometric critic
	// itself runs editor-side (it needs UWorld/traces) and hands its result back via RecordGeometricVerdict.
	// ---------------------------------------------------------------------------------------------
	struct FSceneVerifyState
	{
		FString LevelName;                     // the level this state describes
		bool    bDirty = false;                // a generation ran since the last whole-scene verify
		bool    bEverVerified = false;         // a verdict has been recorded at least once
		bool    bLastVerdictPass = false;      // result of the last recorded geometric verdict
		int32   LastIssueCount = 0;            // geometric issues in the last verdict
		FString LastSummary;                   // one-line human-readable verdict summary
		double  GeneratedTimeSeconds = 0.0;    // when the scene last went dirty
		double  VerifiedTimeSeconds = 0.0;     // when the last verdict was recorded
	};

	// A generation tool ran in CurrentLevelName — mark the scene as needing whole-scene verification.
	// Does NOT itself produce a verdict; an inline per-batch critic is advisory only. If the dirty
	// state belonged to a different level, it is reset to this one (level switch = fresh slate).
	UEBRIDGEMCPCORE_API void MarkSceneDirty(const FString& CurrentLevelName);

	// Record a whole-scene geometric verdict for CurrentLevelName. Clears the dirty flag; this is the
	// ONLY thing that resolves the gate (an inline place_meshes verdict deliberately does not, so a
	// single clean batch can't unlock a scene that still has junk elsewhere).
	UEBRIDGEMCPCORE_API void RecordGeometricVerdict(const FString& CurrentLevelName, bool bPass, int32 IssueCount, const FString& Summary);

	// Gate predicate for "commit / move on" chokepoints (save_current_level, save_all_dirty). Returns
	// true (= BLOCK) when, for CurrentLevelName, the scene has unverified changes OR its last verdict
	// was a FAIL; OutReason explains and points at verify_scene. Returns false (= allow) when the
	// scene is clean, verified-pass, or the state is for a different level.
	UEBRIDGEMCPCORE_API bool HasUnresolvedSceneVerdict(const FString& CurrentLevelName, FString& OutReason);

	// Copy of the verify state (bDirty==false & bEverVerified==false if nothing tracked yet).
	UEBRIDGEMCPCORE_API FSceneVerifyState GetSceneVerifyState();
}
