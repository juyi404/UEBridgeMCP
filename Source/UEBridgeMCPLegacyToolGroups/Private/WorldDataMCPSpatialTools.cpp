#include "WorldDataMCPSpatialTools.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataSceneBriefStore.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InstancedFoliageActor.h"
#include "LevelEditorViewport.h"
#include "Math/RotationMatrix.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ShowFlags.h"
#include "TextureResource.h"

#include <initializer_list>

namespace WorldDataMCP
{
namespace SpatialTools
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

	// An axis on a world-spanning actor (sky/atmosphere) has bounds near HALF_WORLD_MAX.
	// 50 km is far past any hand-built level yet well under those sentinels, so it cleanly
	// separates "huge but real" (a 2 km landscape) from "effectively infinite".
	constexpr double WorldSpanningAxisThreshold = 5.0e6;

	TArray<TSharedPtr<FJsonValue>> MakeIntVecArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(V.X)));
		Out.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(V.Y)));
		Out.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(V.Z)));
		return Out;
	}

	// Accept a vector as either [x,y,z] or {x,y,z}. Returns false (OutVector untouched) when absent.
	bool TryGetVec(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FVector& OutVector)
	{
		if (!Args.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
		{
			OutVector = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Args->TryGetObjectField(Field, Obj) && Obj && Obj->IsValid())
		{
			double X = OutVector.X, Y = OutVector.Y, Z = OutVector.Z;
			(*Obj)->TryGetNumberField(TEXT("x"), X);
			(*Obj)->TryGetNumberField(TEXT("y"), Y);
			(*Obj)->TryGetNumberField(TEXT("z"), Z);
			OutVector = FVector(X, Y, Z);
			return true;
		}
		return false;
	}

	bool TryGetRot(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, FRotator& OutRotation)
	{
		if (!Args.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
		{
			OutRotation = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
			return true;
		}
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Args->TryGetObjectField(Field, Obj) && Obj && Obj->IsValid())
		{
			double Pitch = OutRotation.Pitch, Yaw = OutRotation.Yaw, Roll = OutRotation.Roll;
			(*Obj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*Obj)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*Obj)->TryGetNumberField(TEXT("roll"), Roll);
			OutRotation = FRotator(Pitch, Yaw, Roll);
			return true;
		}
		return false;
	}

	TSharedRef<FJsonObject> RotToJson(const FRotator& R)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	}

	TSharedRef<FJsonObject> RectToJson(double MinX, double MinY, double MaxX, double MaxY)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), FMath::RoundToInt(MinX));
		O->SetNumberField(TEXT("y"), FMath::RoundToInt(MinY));
		O->SetNumberField(TEXT("w"), FMath::RoundToInt(FMath::Max(0.0, MaxX - MinX)));
		O->SetNumberField(TEXT("h"), FMath::RoundToInt(FMath::Max(0.0, MaxY - MinY)));
		return O;
	}

	bool ParseCollisionChannel(const FString& Name, ECollisionChannel& OutChannel)
	{
		if (Name.IsEmpty() || Name.Equals(TEXT("visibility"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Visibility;
			return true;
		}
		if (Name.Equals(TEXT("camera"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Camera;
			return true;
		}
		if (Name.Equals(TEXT("worldStatic"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_WorldStatic;
			return true;
		}
		if (Name.Equals(TEXT("worldDynamic"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_WorldDynamic;
			return true;
		}
		if (Name.Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_Pawn;
			return true;
		}
		if (Name.Equals(TEXT("physicsBody"), ESearchCase::IgnoreCase))
		{
			OutChannel = ECC_PhysicsBody;
			return true;
		}
		return false;
	}

	FLevelEditorViewportClient* GetActivePerspectiveViewportClient()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		FLevelEditorViewportClient* Best = nullptr;
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective())
			{
				Best = Client;
				if (Client->Viewport == GEditor->GetActiveViewport())
				{
					break;
				}
			}
		}
		return Best;
	}

	AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (IsValid(Actor)
				&& (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase)
					|| Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase)))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	FString BuildActorSemanticText(const AActor* Actor)
	{
		if (!IsValid(Actor))
		{
			return FString();
		}

		FString Text = FString::Printf(TEXT("%s %s %s"), *Actor->GetActorLabel(), *Actor->GetName(), *Actor->GetClass()->GetName());
		const FName FolderPath = Actor->GetFolderPath();
		if (!FolderPath.IsNone())
		{
			Text += TEXT(" ");
			Text += FolderPath.ToString();
		}
		for (const FName& Tag : Actor->Tags)
		{
			Text += TEXT(" ");
			Text += Tag.ToString();
		}

		TArray<UStaticMeshComponent*> StaticMeshComponents;
		Actor->GetComponents<UStaticMeshComponent>(StaticMeshComponents);
		for (const UStaticMeshComponent* Component : StaticMeshComponents)
		{
			if (Component && Component->GetStaticMesh())
			{
				Text += TEXT(" ");
				Text += Component->GetStaticMesh()->GetName();
			}
		}

		Text.ToLowerInline();
		return Text;
	}

	bool SemanticTextHasAny(const FString& Text, std::initializer_list<const TCHAR*> Tokens)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Text.Contains(Token))
			{
				return true;
			}
		}
		return false;
	}

	// Semantic road sub-zone: tells a road-CENTRE surface from a road-EDGE / SHOULDER one.
	// This is the semantic layer the agent can't get from geometry alone — adjacent road bands
	// share the same category and near-identical bounds, so the distinction must come from
	// authored signal (naming / tags / folder / mesh names), which the generator bakes in.
	// Caller should only invoke this for actors already classified as "road". Returns "" if unknown.
	FString ClassifyRoadZone(const FString& SemanticText)
	{
		// Most specific first; an asset may carry several tokens (e.g. "road_centre_edge_trim").
		if (SemanticTextHasAny(SemanticText, { TEXT("shoulder"), TEXT("verge"), TEXT("roadside"), TEXT("路肩") }))
		{
			return TEXT("shoulder");
		}
		if (SemanticTextHasAny(SemanticText, { TEXT("curb"), TEXT("kerb"), TEXT("sidewalk"), TEXT("gutter"), TEXT("edge"), TEXT("路边"), TEXT("路缘") }))
		{
			return TEXT("edge");
		}
		if (SemanticTextHasAny(SemanticText, { TEXT("center"), TEXT("centre"), TEXT("centerline"), TEXT("centreline"), TEXT("median"), TEXT("lane"), TEXT("carriageway"), TEXT("roadway"), TEXT("路中"), TEXT("中线"), TEXT("中央") }))
		{
			return TEXT("center");
		}
		return FString();
	}

	// Compact per-actor record for spatial result lists: identity + category + world centre,
	// plus its footprint size so the agent can judge extent without a second call.
	TSharedRef<FJsonObject> MakeSpatialActorObject(const AActor* Actor, const FBox& Bounds)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("label"), Actor->GetActorLabel());
		O->SetStringField(TEXT("name"), Actor->GetName());
		O->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		const FString Category = ClassifyActor(Actor);
		O->SetStringField(TEXT("category"), Category);
		// Surface the road cross-section role so the agent can distinguish road-edge from
		// road-centre surfaces. Only emitted for roads that carry a recognisable zone signal.
		if (Category == TEXT("road"))
		{
			const FString RoadZone = ClassifyRoadZone(BuildActorSemanticText(Actor));
			if (!RoadZone.IsEmpty())
			{
				O->SetStringField(TEXT("roadZone"), RoadZone);
			}
		}
		const FVector Center = Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
		O->SetArrayField(TEXT("center"), MakeIntVecArray(Center));
		if (Bounds.IsValid)
		{
			O->SetArrayField(TEXT("size"), MakeIntVecArray(Bounds.GetSize()));
		}
		return O;
	}

	// Category for a single instanced-mesh instance — the per-instance analogue of ClassifyActor.
	// An ISM/HISM carries no per-instance label, so the OWNER's semantic text plus the instanced
	// mesh name are the only signal. Mirrors ClassifyActor's keyword cascade; falls back to
	// "staticMesh" since an instance is, at minimum, a static mesh.
	FString ClassifyInstancedMesh(const AActor* Owner, const UStaticMesh* Mesh)
	{
		FString Sem = BuildActorSemanticText(Owner); // already lowercased
		if (Mesh)
		{
			Sem += TEXT(" ");
			Sem += Mesh->GetName().ToLower();
		}
		if (SemanticTextHasAny(Sem, { TEXT("road"), TEXT("street"), TEXT("highway"), TEXT("freeway"), TEXT("lane"), TEXT("asphalt"), TEXT("bridge"), TEXT("sidewalk"), TEXT("curb") })) { return TEXT("road"); }
		if (SemanticTextHasAny(Sem, { TEXT("building"), TEXT("house"), TEXT("wall"), TEXT("roof"), TEXT("floor"), TEXT("ceiling"), TEXT("door"), TEXT("window"), TEXT("facade"), TEXT("room"), TEXT("interior") })) { return TEXT("building"); }
		if (SemanticTextHasAny(Sem, { TEXT("tree"), TEXT("forest"), TEXT("foliage"), TEXT("grass"), TEXT("bush"), TEXT("shrub"), TEXT("plant"), TEXT("leaf"), TEXT("leaves") })) { return TEXT("vegetation"); }
		if (SemanticTextHasAny(Sem, { TEXT("rock"), TEXT("stone"), TEXT("boulder"), TEXT("cliff"), TEXT("mountain") })) { return TEXT("rock"); }
		if (SemanticTextHasAny(Sem, { TEXT("water"), TEXT("river"), TEXT("lake"), TEXT("ocean"), TEXT("sea"), TEXT("pond"), TEXT("pool") })) { return TEXT("water"); }
		if (SemanticTextHasAny(Sem, { TEXT("vehicle"), TEXT("car"), TEXT("truck"), TEXT("bus"), TEXT("bike"), TEXT("motorcycle") })) { return TEXT("vehicle"); }
		return TEXT("staticMesh");
	}

	// Result record for a single instanced-mesh instance (parallel to MakeSpatialActorObject).
	// Labelled "<owner>#<index>" so the agent can address the exact instance downstream.
	TSharedRef<FJsonObject> MakeSpatialInstanceObject(const AActor* Owner, int32 Index, const FString& Category, const FBox& WorldBounds)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		const FString OwnerLabel = Owner ? Owner->GetActorLabel() : TEXT("?");
		O->SetStringField(TEXT("label"), FString::Printf(TEXT("%s#%d"), *OwnerLabel, Index));
		O->SetStringField(TEXT("owner"), OwnerLabel);
		O->SetNumberField(TEXT("instanceIndex"), Index);
		O->SetStringField(TEXT("category"), Category);
		O->SetBoolField(TEXT("instanced"), true);
		const FVector Center = WorldBounds.IsValid ? WorldBounds.GetCenter() : FVector::ZeroVector;
		O->SetArrayField(TEXT("center"), MakeIntVecArray(Center));
		if (WorldBounds.IsValid)
		{
			O->SetArrayField(TEXT("size"), MakeIntVecArray(WorldBounds.GetSize()));
		}
		return O;
	}

	struct FProjectionContext
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Forward = FVector::ForwardVector;
		FVector Right = FVector::RightVector;
		FVector Up = FVector::UpVector;
		double HorizontalTan = 1.0;
		double VerticalTan = 1.0;
		double Width = 1920.0;
		double Height = 1080.0;
	};

	FProjectionContext MakeProjectionContext(const FVector& CameraLocation, const FRotator& CameraRotation, double FovDegrees, double Width, double Height)
	{
		FProjectionContext Context;
		Context.Location = CameraLocation;
		Context.Rotation = CameraRotation;
		const FMatrix RotMat = FRotationMatrix(CameraRotation);
		Context.Forward = RotMat.GetScaledAxis(EAxis::X);
		Context.Right = RotMat.GetScaledAxis(EAxis::Y);
		Context.Up = RotMat.GetScaledAxis(EAxis::Z);
		Context.Width = FMath::Max(1.0, Width);
		Context.Height = FMath::Max(1.0, Height);

		const double Aspect = Context.Width / Context.Height;
		const double HorizontalFovRad = FMath::DegreesToRadians(FMath::Clamp(FovDegrees, 5.0, 170.0));
		Context.HorizontalTan = FMath::Tan(HorizontalFovRad * 0.5);
		Context.VerticalTan = Context.HorizontalTan / FMath::Max(Aspect, 0.001);
		return Context;
	}

	bool ProjectPoint(const FProjectionContext& Context, const FVector& Point, FVector2D& OutPixel, double& OutDepth, FVector2D& OutNdc)
	{
		const FVector Delta = Point - Context.Location;
		OutDepth = FVector::DotProduct(Delta, Context.Forward);
		if (OutDepth <= 1.0)
		{
			return false;
		}
		const double X = FVector::DotProduct(Delta, Context.Right) / (OutDepth * Context.HorizontalTan);
		const double Y = FVector::DotProduct(Delta, Context.Up) / (OutDepth * Context.VerticalTan);
		OutNdc = FVector2D(X, Y);
		OutPixel = FVector2D((X + 1.0) * 0.5 * Context.Width, (1.0 - Y) * 0.5 * Context.Height);
		return true;
	}

	TArray<FVector> GetBoxCorners(const FBox& Box)
	{
		TArray<FVector> Corners;
		if (!Box.IsValid)
		{
			return Corners;
		}
		Corners.Reserve(8);
		for (int32 X = 0; X < 2; ++X)
		{
			for (int32 Y = 0; Y < 2; ++Y)
			{
				for (int32 Z = 0; Z < 2; ++Z)
				{
					Corners.Add(FVector(
						X == 0 ? Box.Min.X : Box.Max.X,
						Y == 0 ? Box.Min.Y : Box.Max.Y,
						Z == 0 ? Box.Min.Z : Box.Max.Z));
				}
			}
		}
		return Corners;
	}

	bool ProjectBounds(const FProjectionContext& Context, const FBox& Bounds, double MarginNdc, FVector2D& OutCenterPixel, double& OutDepth, FVector2D& OutMinPixel, FVector2D& OutMaxPixel)
	{
		FVector2D CenterNdc;
		if (!ProjectPoint(Context, Bounds.GetCenter(), OutCenterPixel, OutDepth, CenterNdc))
		{
			return false;
		}

		bool bAnyCornerInFrame = FMath::Abs(CenterNdc.X) <= 1.0 + MarginNdc && FMath::Abs(CenterNdc.Y) <= 1.0 + MarginNdc;
		OutMinPixel = FVector2D(Context.Width, Context.Height);
		OutMaxPixel = FVector2D::ZeroVector;
		for (const FVector& Corner : GetBoxCorners(Bounds))
		{
			FVector2D Pixel, Ndc;
			double Depth = 0.0;
			if (!ProjectPoint(Context, Corner, Pixel, Depth, Ndc))
			{
				continue;
			}
			bAnyCornerInFrame = bAnyCornerInFrame || (FMath::Abs(Ndc.X) <= 1.0 + MarginNdc && FMath::Abs(Ndc.Y) <= 1.0 + MarginNdc);
			OutMinPixel.X = FMath::Min(OutMinPixel.X, Pixel.X);
			OutMinPixel.Y = FMath::Min(OutMinPixel.Y, Pixel.Y);
			OutMaxPixel.X = FMath::Max(OutMaxPixel.X, Pixel.X);
			OutMaxPixel.Y = FMath::Max(OutMaxPixel.Y, Pixel.Y);
		}

		OutMinPixel.X = FMath::Clamp(OutMinPixel.X, 0.0, Context.Width);
		OutMinPixel.Y = FMath::Clamp(OutMinPixel.Y, 0.0, Context.Height);
		OutMaxPixel.X = FMath::Clamp(OutMaxPixel.X, 0.0, Context.Width);
		OutMaxPixel.Y = FMath::Clamp(OutMaxPixel.Y, 0.0, Context.Height);
		if (OutMaxPixel.X <= OutMinPixel.X || OutMaxPixel.Y <= OutMinPixel.Y)
		{
			OutMinPixel = OutCenterPixel;
			OutMaxPixel = OutCenterPixel;
		}
		return bAnyCornerInFrame;
	}
}

// ---------------------------------------------------------------------------------------------
// Shared helpers (also consumed by describe_scene and the actor listings).
// ---------------------------------------------------------------------------------------------

FBox GetActorWorldBounds(const AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return FBox(ForceInit);
	}
	// bNonColliding=true so visual-only meshes count; include child-actor components too.
	return Actor->GetComponentsBoundingBox(/*bNonColliding=*/true, /*bIncludeFromChildActors=*/true);
}

bool IsSpatiallyMeaningfulActor(const AActor* Actor, const FBox& ActorBounds)
{
	if (!IsValid(Actor) || !ActorBounds.IsValid)
	{
		return false;
	}
	const FVector Size = ActorBounds.GetSize();
	if (FMath::Max3(Size.X, Size.Y, Size.Z) > WorldSpanningAxisThreshold)
	{
		return false;
	}
	return true;
}

FBox ComputeLevelBounds(UWorld* World, int32& OutConsidered)
{
	OutConsidered = 0;
	FBox Level(ForceInit);
	if (!World)
	{
		return Level;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		const FBox Bounds = GetActorWorldBounds(Actor);
		if (!IsSpatiallyMeaningfulActor(Actor, Bounds))
		{
			continue;
		}
		Level += Bounds;
		++OutConsidered;
	}
	return Level;
}

TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	if (!Box.IsValid)
	{
		return O;
	}
	O->SetArrayField(TEXT("min"), MakeIntVecArray(Box.Min));
	O->SetArrayField(TEXT("max"), MakeIntVecArray(Box.Max));
	O->SetArrayField(TEXT("center"), MakeIntVecArray(Box.GetCenter()));
	O->SetArrayField(TEXT("size"), MakeIntVecArray(Box.GetSize()));
	return O;
}

FString ClassifyActor(const AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return TEXT("actor");
	}
	const FString ClassName = Actor->GetClass()->GetName();
	const FString SemanticText = BuildActorSemanticText(Actor);

	if (Actor->FindComponentByClass<ULightComponent>())
	{
		return TEXT("light");
	}
	if (ClassName.Contains(TEXT("Camera")) || SemanticTextHasAny(SemanticText, { TEXT("camera"), TEXT("cinecamera") }))
	{
		return TEXT("camera");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("road"), TEXT("street"), TEXT("highway"), TEXT("freeway"), TEXT("lane"), TEXT("asphalt"), TEXT("bridge"), TEXT("sidewalk"), TEXT("curb") }))
	{
		return TEXT("road");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("building"), TEXT("house"), TEXT("wall"), TEXT("roof"), TEXT("floor"), TEXT("ceiling"), TEXT("door"), TEXT("window"), TEXT("facade"), TEXT("room"), TEXT("interior") }))
	{
		return TEXT("building");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("tree"), TEXT("forest"), TEXT("foliage"), TEXT("grass"), TEXT("bush"), TEXT("shrub"), TEXT("plant"), TEXT("leaf"), TEXT("leaves") }))
	{
		return TEXT("vegetation");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("rock"), TEXT("stone"), TEXT("boulder"), TEXT("cliff"), TEXT("mountain") }))
	{
		return TEXT("rock");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("water"), TEXT("river"), TEXT("lake"), TEXT("ocean"), TEXT("sea"), TEXT("pond"), TEXT("pool") }))
	{
		return TEXT("water");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("vehicle"), TEXT("car"), TEXT("truck"), TEXT("bus"), TEXT("bike"), TEXT("motorcycle") }))
	{
		return TEXT("vehicle");
	}
	if (SemanticTextHasAny(SemanticText, { TEXT("character"), TEXT("pawn"), TEXT("player"), TEXT("enemy"), TEXT("npc"), TEXT("crowd"), TEXT("skeletal") }))
	{
		return TEXT("character");
	}
	if (ClassName.Contains(TEXT("Landscape")))
	{
		return TEXT("landscape");
	}
	if (ClassName.Contains(TEXT("Foliage")))
	{
		return TEXT("foliage");
	}
	if (Actor->IsA<AVolume>())
	{
		return TEXT("volume");
	}
	if (Actor->FindComponentByClass<USplineComponent>())
	{
		return TEXT("spline");
	}
	if (Actor->FindComponentByClass<USkeletalMeshComponent>())
	{
		return TEXT("character");
	}
	if (Actor->FindComponentByClass<UStaticMeshComponent>())
	{
		return TEXT("staticMesh");
	}
	return TEXT("actor");
}

void ForEachWorldInstance(UWorld* World, TFunctionRef<void(const AActor*, int32, const FBox&, const FString&)> Visit)
{
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}
		// bIncludeFromChildActors catches ISMs nested under child actors; HISM derives from ISM so
		// foliage HierarchicalISMs are returned by the same query.
		TArray<UInstancedStaticMeshComponent*> Comps;
		Actor->GetComponents<UInstancedStaticMeshComponent>(Comps, /*bIncludeFromChildActors*/ true);
		for (UInstancedStaticMeshComponent* Comp : Comps)
		{
			if (!IsValid(Comp))
			{
				continue;
			}
			UStaticMesh* Mesh = Comp->GetStaticMesh();
			if (!Mesh)
			{
				continue;
			}
			const FBoxSphereBounds LocalBounds = Mesh->GetBounds();
			const FString Category = ClassifyInstancedMesh(Actor, Mesh);
			const int32 Count = Comp->GetInstanceCount();
			for (int32 i = 0; i < Count; ++i)
			{
				FTransform InstanceXform;
				if (!Comp->GetInstanceTransform(i, InstanceXform, /*bWorldSpace*/ true))
				{
					continue;
				}
				Visit(Actor, i, LocalBounds.TransformBy(InstanceXform).GetBox(), Category);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Unified placed-object model (A2): ONE abstraction over the four parallel "scatter routes" —
// StaticMeshActor (place_meshes/hand) · PCG-ISM · foliage-HISM · plain ISM (lay_meshes). Lets
// perception, read-back and generation-time pruning treat the whole scene as a single set of
// placed units instead of four disjoint worlds.
// ---------------------------------------------------------------------------------------------

enum class EPlacementSource : uint8 { Actor, PCG, Foliage, ISM };

static const TCHAR* PlacementSourceToString(EPlacementSource Source)
{
	switch (Source)
	{
	case EPlacementSource::Actor:   return TEXT("actor");
	case EPlacementSource::PCG:     return TEXT("pcg");
	case EPlacementSource::Foliage: return TEXT("foliage");
	default:                        return TEXT("ism");
	}
}

// Provenance of an instanced-mesh component without a hard PCG module link (SpatialTools needs no
// PCG dependency): foliage HISMs hang off an AInstancedFoliageActor; PCG-generated ISMs hang off the
// actor that owns the UPCGComponent (detected by class/label name). Everything else is a plain ISM.
static EPlacementSource ClassifyInstanceSource(const AActor* Owner)
{
	if (!Owner)
	{
		return EPlacementSource::ISM;
	}
	if (Owner->IsA<AInstancedFoliageActor>())
	{
		return EPlacementSource::Foliage;
	}
	if (Owner->GetClass()->GetName().Contains(TEXT("PCG")) || Owner->GetActorLabel().Contains(TEXT("PCG")))
	{
		return EPlacementSource::PCG;
	}
	return EPlacementSource::ISM;
}

// One uniform record for a placed unit, whatever route produced it.
static TSharedRef<FJsonObject> MakePlacementObject(EPlacementSource Source, const AActor* Owner, int32 Index, const FString& Category, const FString& MeshName, const FBox& Bounds)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	const FString OwnerLabel = Owner ? Owner->GetActorLabel() : TEXT("?");
	O->SetStringField(TEXT("source"), PlacementSourceToString(Source));
	O->SetStringField(TEXT("label"), Index < 0 ? OwnerLabel : FString::Printf(TEXT("%s#%d"), *OwnerLabel, Index));
	O->SetStringField(TEXT("owner"), OwnerLabel);
	if (Index >= 0)
	{
		O->SetNumberField(TEXT("instanceIndex"), Index);
	}
	O->SetBoolField(TEXT("instanced"), Index >= 0);
	O->SetStringField(TEXT("category"), Category);
	if (!MeshName.IsEmpty())
	{
		O->SetStringField(TEXT("mesh"), MeshName);
	}
	const FVector Center = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
	O->SetArrayField(TEXT("center"), MakeIntVecArray(Center));
	if (Bounds.IsValid)
	{
		O->SetArrayField(TEXT("size"), MakeIntVecArray(Bounds.GetSize()));
	}
	return O;
}

// The unified enumeration: visit EVERY placed mesh in the world as one uniform unit (source, owner,
// instance index, world AABB, category, mesh), regardless of representation. ISM/HISM instances are
// expanded; non-instanced StaticMesh actors are emitted once; world-spanning infra is skipped.
static void ForEachPlacement(UWorld* World, TFunctionRef<void(EPlacementSource, const AActor*, int32, const FBox&, const FString&, const FString&)> Visit)
{
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		// (1) Instanced content — PCG-ISM / foliage-HISM / plain ISM.
		TArray<UInstancedStaticMeshComponent*> InstComps;
		Actor->GetComponents<UInstancedStaticMeshComponent>(InstComps, /*bIncludeFromChildActors*/ true);
		const EPlacementSource InstSource = ClassifyInstanceSource(Actor);
		for (UInstancedStaticMeshComponent* Comp : InstComps)
		{
			if (!IsValid(Comp))
			{
				continue;
			}
			UStaticMesh* Mesh = Comp->GetStaticMesh();
			if (!Mesh)
			{
				continue;
			}
			const FBoxSphereBounds LocalBounds = Mesh->GetBounds();
			const FString MeshName = Mesh->GetName();
			const FString Category = ClassifyInstancedMesh(Actor, Mesh);
			const int32 Count = Comp->GetInstanceCount();
			for (int32 i = 0; i < Count; ++i)
			{
				FTransform InstanceXform;
				if (!Comp->GetInstanceTransform(i, InstanceXform, /*bWorldSpace*/ true))
				{
					continue;
				}
				Visit(InstSource, Actor, i, LocalBounds.TransformBy(InstanceXform).GetBox(), Category, MeshName);
			}
		}

		// (2) Non-instanced StaticMesh actors — place_meshes output / hand-placed. Skip world-spanning
		// infra so we count discrete placed meshes, not the landscape; exclude ISMs (handled above).
		const FBox ActorBounds = GetActorWorldBounds(Actor);
		if (!IsSpatiallyMeaningfulActor(Actor, ActorBounds))
		{
			continue;
		}
		TArray<UStaticMeshComponent*> MeshComps;
		Actor->GetComponents<UStaticMeshComponent>(MeshComps);
		for (UStaticMeshComponent* MeshComp : MeshComps)
		{
			if (!IsValid(MeshComp) || MeshComp->IsA<UInstancedStaticMeshComponent>())
			{
				continue;
			}
			UStaticMesh* Mesh = MeshComp->GetStaticMesh();
			if (!Mesh)
			{
				continue;
			}
			Visit(EPlacementSource::Actor, Actor, -1, MeshComp->Bounds.GetBox(), ClassifyActor(Actor), Mesh->GetName());
		}
	}
}

void GatherPlacementBounds(UWorld* World, TArray<FBox>& OutBounds, const FBox& RegionFilter)
{
	const bool bFilter = RegionFilter.IsValid != 0;
	ForEachPlacement(World, [&](EPlacementSource, const AActor*, int32, const FBox& Bounds, const FString&, const FString&)
	{
		if (!Bounds.IsValid)
		{
			return;
		}
		if (bFilter && !RegionFilter.Intersect(Bounds))
		{
			return;
		}
		OutBounds.Add(Bounds);
	});
}

TArray<TSharedPtr<FJsonValue>> BuildRegionSummaries(UWorld* World, const FBox& LevelBounds, int32 GridDivisions, int32 MaxRegions)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	if (!World || !LevelBounds.IsValid)
	{
		return Out;
	}

	GridDivisions = FMath::Clamp(GridDivisions, 1, 16);
	const FVector Origin = LevelBounds.Min;
	const FVector Size = LevelBounds.GetSize();
	const double SpanX = FMath::Max(Size.X, 1.0);
	const double SpanY = FMath::Max(Size.Y, 1.0);

	struct FCell
	{
		int32 Count = 0;
		FBox Bounds = FBox(ForceInit);
		TMap<FString, int32> Categories;
		TArray<FString> Samples;
	};
	TMap<int32, FCell> Cells;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		const FBox Bounds = GetActorWorldBounds(Actor);
		if (!IsSpatiallyMeaningfulActor(Actor, Bounds))
		{
			continue;
		}
		const FVector C = Bounds.GetCenter();
		const int32 Cx = FMath::Clamp(static_cast<int32>((C.X - Origin.X) / SpanX * GridDivisions), 0, GridDivisions - 1);
		const int32 Cy = FMath::Clamp(static_cast<int32>((C.Y - Origin.Y) / SpanY * GridDivisions), 0, GridDivisions - 1);
		FCell& Cell = Cells.FindOrAdd(Cy * GridDivisions + Cx);
		++Cell.Count;
		Cell.Bounds += Bounds;
		Cell.Categories.FindOrAdd(ClassifyActor(Actor))++;
		if (Cell.Samples.Num() < 3)
		{
			Cell.Samples.Add(Actor->GetActorLabel());
		}
	}

	// Fold instanced scatter (PCG-ISM / foliage-HISM / lay-spline ISM) into the same density grid.
	// Without this a PCG volume holding 500 trees registers as ONE actor and the digest stays blind to
	// exactly the procedural content this project generates the most of.
	ForEachWorldInstance(World, [&](const AActor* /*Owner*/, int32 /*Index*/, const FBox& InstBounds, const FString& Category)
	{
		const FVector IC = InstBounds.GetCenter();
		const int32 Cx = FMath::Clamp(static_cast<int32>((IC.X - Origin.X) / SpanX * GridDivisions), 0, GridDivisions - 1);
		const int32 Cy = FMath::Clamp(static_cast<int32>((IC.Y - Origin.Y) / SpanY * GridDivisions), 0, GridDivisions - 1);
		FCell& Cell = Cells.FindOrAdd(Cy * GridDivisions + Cx);
		++Cell.Count;
		Cell.Bounds += InstBounds;
		Cell.Categories.FindOrAdd(Category)++;
	});

	// Densest cells first; a handful of labelled regions is what the agent can actually hold.
	Cells.ValueSort([](const FCell& A, const FCell& B) { return A.Count > B.Count; });

	int32 Emitted = 0;
	for (const TPair<int32, FCell>& Pair : Cells)
	{
		if (Emitted >= MaxRegions)
		{
			break;
		}
		const FCell& Cell = Pair.Value;

		FString Dominant;
		int32 DominantCount = -1;
		TArray<TSharedPtr<FJsonValue>> CategoryList;
		for (const TPair<FString, int32>& Cat : Cell.Categories)
		{
			if (Cat.Value > DominantCount)
			{
				DominantCount = Cat.Value;
				Dominant = Cat.Key;
			}
			TSharedRef<FJsonObject> CatObj = MakeShared<FJsonObject>();
			CatObj->SetStringField(TEXT("category"), Cat.Key);
			CatObj->SetNumberField(TEXT("count"), Cat.Value);
			CategoryList.Add(MakeShared<FJsonValueObject>(CatObj));
		}

		TArray<TSharedPtr<FJsonValue>> SampleList;
		for (const FString& Label : Cell.Samples)
		{
			SampleList.Add(MakeShared<FJsonValueString>(Label));
		}

		TSharedRef<FJsonObject> Region = MakeShared<FJsonObject>();
		Region->SetNumberField(TEXT("actorCount"), Cell.Count);
		Region->SetStringField(TEXT("dominantCategory"), Dominant);
		Region->SetArrayField(TEXT("center"), MakeIntVecArray(Cell.Bounds.GetCenter()));
		Region->SetArrayField(TEXT("size"), MakeIntVecArray(Cell.Bounds.GetSize()));
		Region->SetArrayField(TEXT("categories"), CategoryList);
		Region->SetArrayField(TEXT("sampleActors"), SampleList);
		Out.Add(MakeShared<FJsonValueObject>(Region));
		++Emitted;
	}
	return Out;
}

// ---------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------

namespace
{
	FString QueryActorsInRegion(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector Center = FVector::ZeroVector;
		const bool bHasCenter = TryGetVec(Args, TEXT("center"), Center);

		FVector ExtentVec = FVector::ZeroVector;
		const bool bHasExtent = TryGetVec(Args, TEXT("extent"), ExtentVec);

		FVector MinV, MaxV;
		const bool bHasMin = TryGetVec(Args, TEXT("min"), MinV);
		const bool bHasMax = TryGetVec(Args, TEXT("max"), MaxV);

		double Radius = 0.0;
		const bool bHasRadius = Args->TryGetNumberField(TEXT("radius"), Radius) && Radius > 0.0;

		FBox Region(ForceInit);
		bool bSphere = false;
		if (bHasMin && bHasMax)
		{
			Region = FBox(MinV, MaxV);
		}
		else if (bHasCenter && bHasExtent)
		{
			Region = FBox(Center - ExtentVec, Center + ExtentVec);
		}
		else if (bHasCenter && bHasRadius)
		{
			bSphere = true;
			Region = FBox(Center - FVector(Radius), Center + FVector(Radius));
		}
		else
		{
			return ErrorJson(TEXT("Provide a region: {center,extent} or {center,radius} or {min,max}."));
		}

		FString ClassFilter;
		Args->TryGetStringField(TEXT("classFilter"), ClassFilter);

		double MaxResultsNumber = 100.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 500);

		bool bIncludeInstances = true;
		Args->TryGetBoolField(TEXT("includeInstances"), bIncludeInstances);

		// Inst == -1 marks a whole actor; Inst >= 0 is an instanced-mesh instance of Actor.
		struct FMatch { const AActor* Actor; int32 Inst; FString Cat; FBox Bounds; double Dist; };
		TArray<FMatch> Matches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FBox Bounds = GetActorWorldBounds(Actor);
			if (!Bounds.IsValid)
			{
				continue;
			}
			const FVector ActorCenter = Bounds.GetCenter();
			if (bSphere)
			{
				if (FVector::Dist(ActorCenter, Center) > Radius)
				{
					continue;
				}
			}
			else if (!Region.Intersect(Bounds))
			{
				continue;
			}
			Matches.Add({ Actor, -1, FString(), Bounds, FVector::Dist(ActorCenter, Region.GetCenter()) });
		}

		// Also surface instanced scatter (PCG-ISM / foliage-HISM / lay-spline ISM) inside the region —
		// otherwise a forest of 500 ISM trees reads as a single owner actor (or nothing at all).
		if (bIncludeInstances)
		{
			ForEachWorldInstance(World, [&](const AActor* Owner, int32 Index, const FBox& WB, const FString& Category)
			{
				if (!ClassFilter.IsEmpty() && (!Owner || !Owner->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase)))
				{
					return;
				}
				const FVector IC = WB.GetCenter();
				if (bSphere)
				{
					if (FVector::Dist(IC, Center) > Radius) { return; }
				}
				else if (!Region.Intersect(WB))
				{
					return;
				}
				Matches.Add({ Owner, Index, Category, WB, FVector::Dist(IC, Region.GetCenter()) });
			});
		}

		Matches.Sort([](const FMatch& A, const FMatch& B) { return A.Dist < B.Dist; });

		TArray<TSharedPtr<FJsonValue>> Actors;
		for (const FMatch& M : Matches)
		{
			if (Actors.Num() >= MaxResults)
			{
				break;
			}
			TSharedRef<FJsonObject> Obj = (M.Inst < 0) ? MakeSpatialActorObject(M.Actor, M.Bounds) : MakeSpatialInstanceObject(M.Actor, M.Inst, M.Cat, M.Bounds);
			Obj->SetNumberField(TEXT("distanceFromCenter"), FMath::RoundToInt(M.Dist));
			Actors.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("matchedCount"), Matches.Num());
		Result->SetNumberField(TEXT("count"), Actors.Num());
		Result->SetObjectField(TEXT("region"), BoxToJson(Region));
		Result->SetArrayField(TEXT("actors"), Actors);
		return SuccessJson(Result);
	}

	FString FindNearestActors(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector Origin = FVector::ZeroVector;
		const AActor* OriginActor = nullptr;
		FString OriginName;
		if (Args->TryGetStringField(TEXT("actor"), OriginName) || Args->TryGetStringField(TEXT("label"), OriginName))
		{
			OriginActor = FindActorByNameOrLabel(World, OriginName);
			if (!OriginActor)
			{
				return ErrorJson(FString::Printf(TEXT("Origin actor not found: %s"), *OriginName));
			}
			const FBox B = GetActorWorldBounds(OriginActor);
			Origin = B.IsValid ? B.GetCenter() : OriginActor->GetActorLocation();
		}
		else if (!TryGetVec(Args, TEXT("point"), Origin))
		{
			return ErrorJson(TEXT("Provide an origin: 'actor' (name/label) or 'point' [x,y,z]."));
		}

		FString ClassFilter;
		Args->TryGetStringField(TEXT("classFilter"), ClassFilter);

		double MaxDistance = 0.0;
		const bool bHasMaxDistance = Args->TryGetNumberField(TEXT("maxDistance"), MaxDistance) && MaxDistance > 0.0;

		double CountNumber = 10.0;
		Args->TryGetNumberField(TEXT("count"), CountNumber);
		const int32 Count = FMath::Clamp(static_cast<int32>(CountNumber), 1, 200);

		bool bIncludeInstances = true;
		Args->TryGetBoolField(TEXT("includeInstances"), bIncludeInstances);

		// Inst == -1 marks a whole actor; Inst >= 0 is an instanced-mesh instance of Actor.
		struct FMatch { const AActor* Actor; int32 Inst; FString Cat; FBox Bounds; double Dist; };
		TArray<FMatch> Matches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor == OriginActor)
			{
				continue;
			}
			if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FBox Bounds = GetActorWorldBounds(Actor);
			if (!Bounds.IsValid)
			{
				continue;
			}
			const double Dist = FVector::Dist(Bounds.GetCenter(), Origin);
			if (bHasMaxDistance && Dist > MaxDistance)
			{
				continue;
			}
			Matches.Add({ Actor, -1, FString(), Bounds, Dist });
		}

		// Also consider instanced scatter (PCG-ISM / foliage-HISM / lay-spline ISM) as nearest
		// candidates — otherwise "what's near here" can't see the bulk of procedural content.
		if (bIncludeInstances)
		{
			ForEachWorldInstance(World, [&](const AActor* Owner, int32 Index, const FBox& WB, const FString& Category)
			{
				if (Owner == OriginActor)
				{
					return;
				}
				if (!ClassFilter.IsEmpty() && (!Owner || !Owner->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase)))
				{
					return;
				}
				const double D = FVector::Dist(WB.GetCenter(), Origin);
				if (bHasMaxDistance && D > MaxDistance)
				{
					return;
				}
				Matches.Add({ Owner, Index, Category, WB, D });
			});
		}

		Matches.Sort([](const FMatch& A, const FMatch& B) { return A.Dist < B.Dist; });

		TArray<TSharedPtr<FJsonValue>> Actors;
		for (const FMatch& M : Matches)
		{
			if (Actors.Num() >= Count)
			{
				break;
			}
			TSharedRef<FJsonObject> Obj = (M.Inst < 0) ? MakeSpatialActorObject(M.Actor, M.Bounds) : MakeSpatialInstanceObject(M.Actor, M.Inst, M.Cat, M.Bounds);
			Obj->SetNumberField(TEXT("distance"), FMath::RoundToInt(M.Dist));
			Actors.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("origin"), MakeIntVecArray(Origin));
		Result->SetNumberField(TEXT("matchedCount"), Matches.Num());
		Result->SetNumberField(TEXT("count"), Actors.Num());
		Result->SetArrayField(TEXT("actors"), Actors);
		return SuccessJson(Result);
	}

	// Unified read-back over the A2 placed-object model: all four routes at once, with provenance.
	FString ListPlacements(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		// Optional region filter — same shapes as query_actors_in_region.
		FBox Region(ForceInit);
		bool bSphere = false;
		FVector Center = FVector::ZeroVector;
		double Radius = 0.0;
		{
			FVector ExtentVec, MinV, MaxV;
			if (TryGetVec(Args, TEXT("min"), MinV) && TryGetVec(Args, TEXT("max"), MaxV))
			{
				Region = FBox(MinV, MaxV);
			}
			else if (TryGetVec(Args, TEXT("center"), Center) && TryGetVec(Args, TEXT("extent"), ExtentVec))
			{
				Region = FBox(Center - ExtentVec, Center + ExtentVec);
			}
			else if (TryGetVec(Args, TEXT("center"), Center) && Args->TryGetNumberField(TEXT("radius"), Radius) && Radius > 0.0)
			{
				bSphere = true;
				Region = FBox(Center - FVector(Radius), Center + FVector(Radius));
			}
		}
		const bool bHasRegion = Region.IsValid != 0;

		FString SourceFilter, CategoryFilter, MeshFilter;
		Args->TryGetStringField(TEXT("source"), SourceFilter);
		Args->TryGetStringField(TEXT("category"), CategoryFilter);
		Args->TryGetStringField(TEXT("mesh"), MeshFilter);

		double MaxResultsNumber = 200.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 2000);
		bool bIncludeList = true;
		Args->TryGetBoolField(TEXT("includeList"), bIncludeList);

		TMap<FString, int32> BySource, ByCategory;
		int32 Total = 0;
		TArray<TSharedPtr<FJsonValue>> Items;
		ForEachPlacement(World, [&](EPlacementSource Source, const AActor* Owner, int32 Index, const FBox& Bounds, const FString& Category, const FString& MeshName)
		{
			if (bHasRegion)
			{
				const FVector C = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
				if (bSphere)
				{
					if (FVector::Dist(C, Center) > Radius) { return; }
				}
				else if (!Region.Intersect(Bounds))
				{
					return;
				}
			}
			const FString SourceStr = PlacementSourceToString(Source);
			if (!SourceFilter.IsEmpty() && !SourceStr.Equals(SourceFilter, ESearchCase::IgnoreCase)) { return; }
			if (!CategoryFilter.IsEmpty() && !Category.Equals(CategoryFilter, ESearchCase::IgnoreCase)) { return; }
			if (!MeshFilter.IsEmpty() && !MeshName.Contains(MeshFilter)) { return; }
			++Total;
			BySource.FindOrAdd(SourceStr)++;
			ByCategory.FindOrAdd(Category)++;
			if (bIncludeList && Items.Num() < MaxResults)
			{
				Items.Add(MakeShared<FJsonValueObject>(MakePlacementObject(Source, Owner, Index, Category, MeshName, Bounds)));
			}
		});

		auto MapToJson = [](const TMap<FString, int32>& M)
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& P : M) { O->SetNumberField(P.Key, P.Value); }
			return O;
		};

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("returned"), Items.Num());
		Result->SetBoolField(TEXT("truncated"), bIncludeList && Total > Items.Num());
		Result->SetObjectField(TEXT("bySource"), MapToJson(BySource));
		Result->SetObjectField(TEXT("byCategory"), MapToJson(ByCategory));
		if (bHasRegion)
		{
			Result->SetObjectField(TEXT("region"), BoxToJson(Region));
		}
		Result->SetArrayField(TEXT("placements"), Items);
		return SuccessJson(Result);
	}

	FString QueryVisibleActorsFromCamera(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector CameraLocation = FVector::ZeroVector;
		FRotator CameraRotation = FRotator::ZeroRotator;
		double Fov = 90.0;
		double Width = 1920.0;
		double Height = 1080.0;
		bool bHaveCamera = false;

		FString CameraActorName;
		if (Args->TryGetStringField(TEXT("cameraActor"), CameraActorName) && !CameraActorName.IsEmpty())
		{
			AActor* CameraActor = FindActorByNameOrLabel(World, CameraActorName);
			if (!CameraActor)
			{
				return ErrorJson(FString::Printf(TEXT("Camera actor not found: %s"), *CameraActorName));
			}
			CameraLocation = CameraActor->GetActorLocation();
			CameraRotation = CameraActor->GetActorRotation();
			bHaveCamera = true;
		}

		if (TryGetVec(Args, TEXT("cameraLocation"), CameraLocation) && TryGetRot(Args, TEXT("cameraRotation"), CameraRotation))
		{
			bHaveCamera = true;
		}

		if (!bHaveCamera)
		{
			FLevelEditorViewportClient* ViewportClient = GetActivePerspectiveViewportClient();
			if (!ViewportClient)
			{
				return ErrorJson(TEXT("No perspective level viewport is available, and no cameraLocation/cameraRotation was supplied."));
			}
			CameraLocation = ViewportClient->GetViewLocation();
			CameraRotation = ViewportClient->GetViewRotation();
			Fov = ViewportClient->ViewFOV;
			if (ViewportClient->Viewport)
			{
				const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
				Width = Size.X > 0 ? Size.X : Width;
				Height = Size.Y > 0 ? Size.Y : Height;
			}
		}

		Args->TryGetNumberField(TEXT("fov"), Fov);
		Args->TryGetNumberField(TEXT("viewportWidth"), Width);
		Args->TryGetNumberField(TEXT("viewportHeight"), Height);

		double MaxDistance = 200000.0;
		Args->TryGetNumberField(TEXT("maxDistance"), MaxDistance);
		double MaxResultsNumber = 100.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 500);

		double ScreenMargin = 0.15;
		Args->TryGetNumberField(TEXT("screenMargin"), ScreenMargin);
		ScreenMargin = FMath::Clamp(ScreenMargin, 0.0, 1.0);

		FString ClassFilter;
		Args->TryGetStringField(TEXT("classFilter"), ClassFilter);
		FString CategoryFilter;
		Args->TryGetStringField(TEXT("categoryFilter"), CategoryFilter);

		bool bIncludeOcclusion = true;
		Args->TryGetBoolField(TEXT("includeOcclusion"), bIncludeOcclusion);

		const FProjectionContext Projection = MakeProjectionContext(CameraLocation, CameraRotation, Fov, Width, Height);

		struct FVisibleActor
		{
			TSharedRef<FJsonObject> Json;
			double Depth = 0.0;
			double AreaFraction = 0.0;
		};
		TArray<FVisibleActor> Visible;
		int32 Candidates = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Category = ClassifyActor(Actor);
			if (!CategoryFilter.IsEmpty() && !Category.Contains(CategoryFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FBox Bounds = GetActorWorldBounds(Actor);
			if (!IsSpatiallyMeaningfulActor(Actor, Bounds))
			{
				continue;
			}
			const double Distance = FVector::Dist(Bounds.GetCenter(), CameraLocation);
			if (MaxDistance > 0.0 && Distance > MaxDistance)
			{
				continue;
			}
			++Candidates;

			FVector2D CenterPixel, MinPixel, MaxPixel;
			double Depth = 0.0;
			if (!ProjectBounds(Projection, Bounds, ScreenMargin, CenterPixel, Depth, MinPixel, MaxPixel))
			{
				continue;
			}

			TSharedRef<FJsonObject> Obj = MakeSpatialActorObject(Actor, Bounds);
			Obj->SetNumberField(TEXT("distance"), FMath::RoundToInt(Distance));
			Obj->SetNumberField(TEXT("depth"), FMath::RoundToInt(Depth));
			TArray<TSharedPtr<FJsonValue>> Pixel;
			Pixel.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(CenterPixel.X)));
			Pixel.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(CenterPixel.Y)));
			Obj->SetArrayField(TEXT("screenCenter"), Pixel);
			Obj->SetObjectField(TEXT("screenRect"), RectToJson(MinPixel.X, MinPixel.Y, MaxPixel.X, MaxPixel.Y));
			const double AreaFraction = ((MaxPixel.X - MinPixel.X) * (MaxPixel.Y - MinPixel.Y)) / FMath::Max(1.0, Width * Height);
			Obj->SetNumberField(TEXT("screenAreaFraction"), AreaFraction);

			if (bIncludeOcclusion)
			{
				FHitResult Hit;
				FCollisionQueryParams Params(FName(TEXT("MCPVisibleActorsOcclusion")), /*bTraceComplex=*/true);
				Params.AddIgnoredActor(Actor);
				const bool bOccluded = World->LineTraceSingleByChannel(Hit, CameraLocation, Bounds.GetCenter(), ECC_Visibility, Params);
				Obj->SetBoolField(TEXT("occluded"), bOccluded);
				if (bOccluded)
				{
					if (const AActor* Occluder = Hit.GetActor())
					{
						Obj->SetStringField(TEXT("occluder"), Occluder->GetActorLabel());
						Obj->SetStringField(TEXT("occluderCategory"), ClassifyActor(Occluder));
					}
					Obj->SetArrayField(TEXT("occlusionPoint"), MakeIntVecArray(Hit.ImpactPoint));
				}
			}

			Visible.Add({ Obj, Depth, AreaFraction });
		}

		Visible.Sort([](const FVisibleActor& A, const FVisibleActor& B)
		{
			if (!FMath::IsNearlyEqual(A.AreaFraction, B.AreaFraction))
			{
				return A.AreaFraction > B.AreaFraction;
			}
			return A.Depth < B.Depth;
		});

		TArray<TSharedPtr<FJsonValue>> Actors;
		for (const FVisibleActor& Entry : Visible)
		{
			if (Actors.Num() >= MaxResults)
			{
				break;
			}
			Actors.Add(MakeShared<FJsonValueObject>(Entry.Json));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("cameraLocation"), MakeIntVecArray(CameraLocation));
		Result->SetObjectField(TEXT("cameraRotation"), RotToJson(CameraRotation));
		Result->SetNumberField(TEXT("fov"), Fov);
		TSharedRef<FJsonObject> Viewport = MakeShared<FJsonObject>();
		Viewport->SetNumberField(TEXT("width"), FMath::RoundToInt(Width));
		Viewport->SetNumberField(TEXT("height"), FMath::RoundToInt(Height));
		Result->SetObjectField(TEXT("viewport"), Viewport);
		Result->SetNumberField(TEXT("candidateCount"), Candidates);
		Result->SetNumberField(TEXT("matchedCount"), Visible.Num());
		Result->SetNumberField(TEXT("count"), Actors.Num());
		Result->SetArrayField(TEXT("actors"), Actors);
		return SuccessJson(Result);
	}

	struct FSpatialActorRecord
	{
		AActor* Actor = nullptr;
		FBox Bounds = FBox(ForceInit);
		FString Category;
		FVector Center = FVector::ZeroVector;
		double Footprint = 0.0;
	};

	double AxisGap(double AMin, double AMax, double BMin, double BMax)
	{
		if (AMax < BMin)
		{
			return BMin - AMax;
		}
		if (BMax < AMin)
		{
			return AMin - BMax;
		}
		return 0.0;
	}

	bool BoxContainsBox(const FBox& Outer, const FBox& Inner)
	{
		return Outer.IsValid && Inner.IsValid
			&& Outer.Min.X <= Inner.Min.X && Outer.Min.Y <= Inner.Min.Y && Outer.Min.Z <= Inner.Min.Z
			&& Outer.Max.X >= Inner.Max.X && Outer.Max.Y >= Inner.Max.Y && Outer.Max.Z >= Inner.Max.Z;
	}

	TSharedRef<FJsonObject> MakeRelationActorJson(const FSpatialActorRecord& Record)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("label"), Record.Actor->GetActorLabel());
		O->SetStringField(TEXT("category"), Record.Category);
		O->SetArrayField(TEXT("center"), MakeIntVecArray(Record.Center));
		O->SetArrayField(TEXT("size"), MakeIntVecArray(Record.Bounds.GetSize()));
		return O;
	}

	FString AnalyzeSpatialRelations(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FString FocusName;
		AActor* FocusActor = nullptr;
		if ((Args->TryGetStringField(TEXT("actor"), FocusName) || Args->TryGetStringField(TEXT("label"), FocusName)) && !FocusName.IsEmpty())
		{
			FocusActor = FindActorByNameOrLabel(World, FocusName);
			if (!FocusActor)
			{
				return ErrorJson(FString::Printf(TEXT("Actor not found: %s"), *FocusName));
			}
		}

		FString ClassFilter;
		Args->TryGetStringField(TEXT("classFilter"), ClassFilter);
		FString CategoryFilter;
		Args->TryGetStringField(TEXT("categoryFilter"), CategoryFilter);

		double MaxActorsNumber = 60.0;
		Args->TryGetNumberField(TEXT("maxActors"), MaxActorsNumber);
		const int32 MaxActors = FMath::Clamp(static_cast<int32>(MaxActorsNumber), 2, 200);

		double MaxRelationsNumber = 200.0;
		Args->TryGetNumberField(TEXT("maxRelations"), MaxRelationsNumber);
		const int32 MaxRelations = FMath::Clamp(static_cast<int32>(MaxRelationsNumber), 1, 1000);

		double NearDistance = 1000.0;
		Args->TryGetNumberField(TEXT("nearDistance"), NearDistance);

		bool bIncludeDirectional = true;
		Args->TryGetBoolField(TEXT("includeDirectional"), bIncludeDirectional);

		FVector RegionCenter = FVector::ZeroVector;
		const bool bHasRegionCenter = TryGetVec(Args, TEXT("center"), RegionCenter);
		double Radius = 0.0;
		const bool bHasRadius = Args->TryGetNumberField(TEXT("radius"), Radius) && Radius > 0.0;

		TArray<FSpatialActorRecord> Records;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}
			if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FBox Bounds = GetActorWorldBounds(Actor);
			if (!IsSpatiallyMeaningfulActor(Actor, Bounds))
			{
				continue;
			}
			const FString Category = ClassifyActor(Actor);
			if (!CategoryFilter.IsEmpty() && !Category.Contains(CategoryFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FVector Center = Bounds.GetCenter();
			if (bHasRegionCenter && bHasRadius && FVector::Dist(Center, RegionCenter) > Radius)
			{
				continue;
			}
			const FVector Size = Bounds.GetSize();
			Records.Add({ Actor, Bounds, Category, Center, static_cast<double>(Size.X) * Size.Y });
		}

		if (FocusActor)
		{
			Records.Sort([FocusActor](const FSpatialActorRecord& A, const FSpatialActorRecord& B)
			{
				if (A.Actor == FocusActor) { return true; }
				if (B.Actor == FocusActor) { return false; }
				return FVector::DistSquared(A.Center, FocusActor->GetActorLocation()) < FVector::DistSquared(B.Center, FocusActor->GetActorLocation());
			});
		}
		else
		{
			Records.Sort([](const FSpatialActorRecord& A, const FSpatialActorRecord& B) { return A.Footprint > B.Footprint; });
		}
		if (Records.Num() > MaxActors)
		{
			Records.SetNum(MaxActors);
		}

		const int32 FocusIndex = FocusActor
			? Records.IndexOfByPredicate([FocusActor](const FSpatialActorRecord& R) { return R.Actor == FocusActor; })
			: INDEX_NONE;
		if (FocusActor && FocusIndex == INDEX_NONE)
		{
			return ErrorJson(TEXT("Focus actor has no meaningful world bounds after filters."));
		}

		struct FRelationEntry
		{
			TSharedRef<FJsonObject> Json;
			double Distance = 0.0;
			bool bImportant = false;
		};
		TArray<FRelationEntry> Relations;

		auto AddRelation = [&](const FSpatialActorRecord& Source, const FSpatialActorRecord& Target)
		{
			const FVector Delta = Target.Center - Source.Center;
			const double CenterDistance = Delta.Size();
			const double GapX = AxisGap(Source.Bounds.Min.X, Source.Bounds.Max.X, Target.Bounds.Min.X, Target.Bounds.Max.X);
			const double GapY = AxisGap(Source.Bounds.Min.Y, Source.Bounds.Max.Y, Target.Bounds.Min.Y, Target.Bounds.Max.Y);
			const double GapZ = AxisGap(Source.Bounds.Min.Z, Source.Bounds.Max.Z, Target.Bounds.Min.Z, Target.Bounds.Max.Z);
			const double EdgeGap = FVector(GapX, GapY, GapZ).Size();
			const bool bOverlaps = Source.Bounds.Intersect(Target.Bounds);
			const bool bSourceContainsTarget = BoxContainsBox(Source.Bounds, Target.Bounds);
			const bool bTargetContainsSource = BoxContainsBox(Target.Bounds, Source.Bounds);
			const bool bNear = EdgeGap <= NearDistance;
			const bool bImportant = bOverlaps || bSourceContainsTarget || bTargetContainsSource || bNear;
			if (!FocusActor && !bImportant)
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> RelationNames;
			if (bOverlaps) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("overlaps"))); }
			if (bSourceContainsTarget) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("sourceContainsTarget"))); }
			if (bTargetContainsSource) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("targetContainsSource"))); }
			if (bNear) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("near"))); }

			if (bIncludeDirectional)
			{
				if (FMath::Abs(Delta.Z) > FMath::Max(75.0, 0.1 * FMath::Max(Source.Bounds.GetSize().Z, Target.Bounds.GetSize().Z)))
				{
					RelationNames.Add(MakeShared<FJsonValueString>(Delta.Z > 0.0 ? TEXT("targetAboveSource") : TEXT("targetBelowSource")));
				}
				if (FMath::Abs(Delta.X) >= FMath::Abs(Delta.Y))
				{
					RelationNames.Add(MakeShared<FJsonValueString>(Delta.X >= 0.0 ? TEXT("targetNorthOfSource") : TEXT("targetSouthOfSource")));
				}
				else
				{
					RelationNames.Add(MakeShared<FJsonValueString>(Delta.Y >= 0.0 ? TEXT("targetEastOfSource") : TEXT("targetWestOfSource")));
				}

				const FVector HorizontalDelta(Delta.X, Delta.Y, 0.0);
				if (!HorizontalDelta.IsNearlyZero())
				{
					const FVector Dir = HorizontalDelta.GetSafeNormal();
					const double SourceFacing = FVector::DotProduct(Source.Actor->GetActorForwardVector().GetSafeNormal2D(), Dir);
					const double TargetFacing = FVector::DotProduct(Target.Actor->GetActorForwardVector().GetSafeNormal2D(), -Dir);
					if (SourceFacing > 0.85)
					{
						RelationNames.Add(MakeShared<FJsonValueString>(TEXT("sourceFacesTarget")));
					}
					if (TargetFacing > 0.85)
					{
						RelationNames.Add(MakeShared<FJsonValueString>(TEXT("targetFacesSource")));
					}
				}
			}

			const double AlignTolerance = 150.0;
			if (FMath::Abs(Delta.X) <= AlignTolerance) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("sameXBand"))); }
			if (FMath::Abs(Delta.Y) <= AlignTolerance) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("sameYBand"))); }
			if (FMath::Abs(Delta.Z) <= AlignTolerance) { RelationNames.Add(MakeShared<FJsonValueString>(TEXT("sameElevation"))); }

			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("source"), MakeRelationActorJson(Source));
			Obj->SetObjectField(TEXT("target"), MakeRelationActorJson(Target));
			Obj->SetArrayField(TEXT("relations"), RelationNames);
			Obj->SetArrayField(TEXT("delta"), MakeIntVecArray(Delta));
			Obj->SetNumberField(TEXT("centerDistance"), FMath::RoundToInt(CenterDistance));
			Obj->SetNumberField(TEXT("edgeGap"), FMath::RoundToInt(EdgeGap));
			Relations.Add({ Obj, CenterDistance, bImportant });
		};

		if (FocusActor)
		{
			const FSpatialActorRecord& Focus = Records[FocusIndex];
			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				if (Index == FocusIndex)
				{
					continue;
				}
				AddRelation(Focus, Records[Index]);
			}
		}
		else
		{
			for (int32 A = 0; A < Records.Num(); ++A)
			{
				for (int32 B = A + 1; B < Records.Num(); ++B)
				{
					AddRelation(Records[A], Records[B]);
				}
			}
		}

		Relations.Sort([](const FRelationEntry& A, const FRelationEntry& B)
		{
			if (A.bImportant != B.bImportant)
			{
				return A.bImportant;
			}
			return A.Distance < B.Distance;
		});

		TArray<TSharedPtr<FJsonValue>> ActorArray;
		for (const FSpatialActorRecord& Record : Records)
		{
			ActorArray.Add(MakeShared<FJsonValueObject>(MakeRelationActorJson(Record)));
		}

		TArray<TSharedPtr<FJsonValue>> RelationArray;
		for (const FRelationEntry& Relation : Relations)
		{
			if (RelationArray.Num() >= MaxRelations)
			{
				break;
			}
			RelationArray.Add(MakeShared<FJsonValueObject>(Relation.Json));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("actorCount"), Records.Num());
		Result->SetArrayField(TEXT("actors"), ActorArray);
		Result->SetNumberField(TEXT("matchedRelationCount"), Relations.Num());
		Result->SetNumberField(TEXT("relationCount"), RelationArray.Num());
		Result->SetArrayField(TEXT("relations"), RelationArray);
		Result->SetStringField(TEXT("orientation"), TEXT("World +X is north, +Y is east, +Z is up."));
		return SuccessJson(Result);
	}

	FString SampleSurfaceGrid(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		FBox Region(ForceInit);
		if (TryGetVec(Args, TEXT("center"), Center) && TryGetVec(Args, TEXT("extent"), Extent))
		{
			Region = FBox(Center - Extent, Center + Extent);
		}
		else
		{
			int32 Considered = 0;
			Region = ComputeLevelBounds(World, Considered);
			if (!Region.IsValid)
			{
				return ErrorJson(TEXT("Scene has no spatially-meaningful actors to sample."));
			}
		}

		double GridXNumber = 5.0;
		Args->TryGetNumberField(TEXT("gridX"), GridXNumber);
		double GridYNumber = 5.0;
		Args->TryGetNumberField(TEXT("gridY"), GridYNumber);
		const int32 GridX = FMath::Clamp(static_cast<int32>(GridXNumber), 1, 32);
		const int32 GridY = FMath::Clamp(static_cast<int32>(GridYNumber), 1, 32);

		double VerticalPadding = 10000.0;
		Args->TryGetNumberField(TEXT("verticalPadding"), VerticalPadding);
		VerticalPadding = FMath::Max(100.0, VerticalPadding);

		FString ChannelName;
		Args->TryGetStringField(TEXT("channel"), ChannelName);
		ECollisionChannel Channel = ECC_Visibility;
		if (!ParseCollisionChannel(ChannelName, Channel))
		{
			return ErrorJson(FString::Printf(TEXT("Unsupported collision channel: %s"), *ChannelName));
		}

		FCollisionQueryParams Params(FName(TEXT("MCPSurfaceGrid")), /*bTraceComplex=*/true);
		Params.bReturnPhysicalMaterial = true;

		TArray<TSharedPtr<FJsonValue>> Samples;
		int32 HitCount = 0;
		double MinZ = TNumericLimits<double>::Max();
		double MaxZ = -TNumericLimits<double>::Max();
		double SlopeSum = 0.0;

		for (int32 Y = 0; Y < GridY; ++Y)
		{
			const double Ty = GridY == 1 ? 0.5 : static_cast<double>(Y) / static_cast<double>(GridY - 1);
			for (int32 X = 0; X < GridX; ++X)
			{
				const double Tx = GridX == 1 ? 0.5 : static_cast<double>(X) / static_cast<double>(GridX - 1);
				const double WorldX = FMath::Lerp(Region.Min.X, Region.Max.X, Tx);
				const double WorldY = FMath::Lerp(Region.Min.Y, Region.Max.Y, Ty);
				const FVector Start(WorldX, WorldY, Region.Max.Z + VerticalPadding);
				const FVector End(WorldX, WorldY, Region.Min.Z - VerticalPadding);

				FHitResult Hit;
				const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, Channel, Params);

				TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetNumberField(TEXT("gridX"), X);
				Sample->SetNumberField(TEXT("gridY"), Y);
				Sample->SetArrayField(TEXT("xy"), MakeIntVecArray(FVector(WorldX, WorldY, 0.0)));
				Sample->SetBoolField(TEXT("hit"), bHit);
				if (bHit)
				{
					++HitCount;
					const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Hit.ImpactNormal.GetSafeNormal(), FVector::UpVector), -1.0, 1.0)));
					MinZ = FMath::Min(MinZ, Hit.ImpactPoint.Z);
					MaxZ = FMath::Max(MaxZ, Hit.ImpactPoint.Z);
					SlopeSum += Slope;

					Sample->SetArrayField(TEXT("point"), MakeIntVecArray(Hit.ImpactPoint));
					Sample->SetArrayField(TEXT("normal"), MakeIntVecArray(Hit.ImpactNormal));
					Sample->SetNumberField(TEXT("slopeDegrees"), Slope);
					if (const AActor* HitActor = Hit.GetActor())
					{
						Sample->SetStringField(TEXT("actor"), HitActor->GetActorLabel());
						Sample->SetStringField(TEXT("category"), ClassifyActor(HitActor));
					}
					if (Hit.PhysMaterial.IsValid())
					{
						Sample->SetStringField(TEXT("physicalMaterial"), Hit.PhysMaterial->GetPathName());
					}
				}
				Samples.Add(MakeShared<FJsonValueObject>(Sample));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("region"), BoxToJson(Region));
		Result->SetNumberField(TEXT("gridX"), GridX);
		Result->SetNumberField(TEXT("gridY"), GridY);
		Result->SetNumberField(TEXT("sampleCount"), Samples.Num());
		Result->SetNumberField(TEXT("hitCount"), HitCount);
		if (HitCount > 0)
		{
			Result->SetNumberField(TEXT("minZ"), FMath::RoundToInt(MinZ));
			Result->SetNumberField(TEXT("maxZ"), FMath::RoundToInt(MaxZ));
			Result->SetNumberField(TEXT("averageSlopeDegrees"), SlopeSum / HitCount);
		}
		Result->SetArrayField(TEXT("samples"), Samples);
		return SuccessJson(Result);
	}

	void AddIgnoredActorsFromArgs(UWorld* World, const TSharedPtr<FJsonObject>& Args, FCollisionQueryParams& Params)
	{
		const TArray<TSharedPtr<FJsonValue>>* IgnoreValues = nullptr;
		if (!World || !Args->TryGetArrayField(TEXT("ignoreActors"), IgnoreValues) || !IgnoreValues)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *IgnoreValues)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name))
			{
				if (AActor* Actor = FindActorByNameOrLabel(World, Name))
				{
					Params.AddIgnoredActor(Actor);
				}
			}
		}
	}

	FString TestPlacementClearance(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector Center = FVector::ZeroVector;
		if (!TryGetVec(Args, TEXT("center"), Center))
		{
			return ErrorJson(TEXT("Missing required 'center' [x,y,z]."));
		}

		FString ShapeName = TEXT("box");
		Args->TryGetStringField(TEXT("shape"), ShapeName);

		FVector Extent(50.0, 50.0, 50.0);
		TryGetVec(Args, TEXT("extent"), Extent);
		double Radius = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
		Args->TryGetNumberField(TEXT("radius"), Radius);
		double HalfHeight = FMath::Max(Extent.Z, Radius);
		Args->TryGetNumberField(TEXT("halfHeight"), HalfHeight);

		FCollisionShape Shape = FCollisionShape::MakeBox(Extent);
		if (ShapeName.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			Shape = FCollisionShape::MakeSphere(FMath::Max(1.0, Radius));
		}
		else if (ShapeName.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
		{
			Shape = FCollisionShape::MakeCapsule(FMath::Max(1.0, Radius), FMath::Max(Radius, HalfHeight));
		}
		else if (!ShapeName.Equals(TEXT("box"), ESearchCase::IgnoreCase))
		{
			return ErrorJson(FString::Printf(TEXT("Unsupported shape '%s'. Use box, sphere, or capsule."), *ShapeName));
		}

		FString ChannelName;
		Args->TryGetStringField(TEXT("channel"), ChannelName);
		ECollisionChannel Channel = ECC_Visibility;
		if (!ParseCollisionChannel(ChannelName, Channel))
		{
			return ErrorJson(FString::Printf(TEXT("Unsupported collision channel: %s"), *ChannelName));
		}

		double MaxResultsNumber = 50.0;
		Args->TryGetNumberField(TEXT("maxResults"), MaxResultsNumber);
		const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsNumber), 1, 200);

		FCollisionQueryParams Params(FName(TEXT("MCPPlacementClearance")), /*bTraceComplex=*/true);
		AddIgnoredActorsFromArgs(World, Args, Params);

		TArray<FOverlapResult> Overlaps;
		World->OverlapMultiByChannel(Overlaps, Center, FQuat::Identity, Channel, Shape, Params);

		TArray<TSharedPtr<FJsonValue>> Blocking;
		int32 BlockingCount = 0;
		for (const FOverlapResult& Overlap : Overlaps)
		{
			if (!Overlap.bBlockingHit)
			{
				continue;
			}
			++BlockingCount;
			if (Blocking.Num() >= MaxResults)
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			if (const AActor* Actor = Overlap.GetActor())
			{
				Entry->SetStringField(TEXT("actor"), Actor->GetActorLabel());
				Entry->SetStringField(TEXT("category"), ClassifyActor(Actor));
				Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			}
			if (const UPrimitiveComponent* Component = Overlap.GetComponent())
			{
				Entry->SetStringField(TEXT("component"), Component->GetName());
			}
			Blocking.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("center"), MakeIntVecArray(Center));
		Result->SetStringField(TEXT("shape"), ShapeName);
		Result->SetBoolField(TEXT("clear"), BlockingCount == 0);
		Result->SetNumberField(TEXT("blockingOverlapCount"), BlockingCount);
		Result->SetBoolField(TEXT("truncated"), BlockingCount > Blocking.Num());
		Result->SetArrayField(TEXT("blockingOverlaps"), Blocking);
		return SuccessJson(Result);
	}

	FString AnalyzeNavigationPath(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!NavSys)
		{
			return ErrorJson(TEXT("No navigation system in the current world."));
		}

		FVector Start = FVector::ZeroVector;
		if (!TryGetVec(Args, TEXT("start"), Start))
		{
			return ErrorJson(TEXT("Missing required 'start' [x,y,z]."));
		}
		FVector End = FVector::ZeroVector;
		if (!TryGetVec(Args, TEXT("end"), End))
		{
			return ErrorJson(TEXT("Missing required 'end' [x,y,z]."));
		}

		bool bProjectToNav = true;
		Args->TryGetBoolField(TEXT("projectToNav"), bProjectToNav);
		bool bIncludePathPoints = true;
		Args->TryGetBoolField(TEXT("includePathPoints"), bIncludePathPoints);

		FVector ProjectionExtent(500.0, 500.0, 1000.0);
		TryGetVec(Args, TEXT("projectionExtent"), ProjectionExtent);

		FVector NavStart = Start;
		FVector NavEnd = End;
		bool bStartProjected = true;
		bool bEndProjected = true;
		if (bProjectToNav)
		{
			FNavLocation ProjectedStart;
			bStartProjected = NavSys->ProjectPointToNavigation(Start, ProjectedStart, ProjectionExtent);
			if (bStartProjected)
			{
				NavStart = ProjectedStart.Location;
			}
			FNavLocation ProjectedEnd;
			bEndProjected = NavSys->ProjectPointToNavigation(End, ProjectedEnd, ProjectionExtent);
			if (bEndProjected)
			{
				NavEnd = ProjectedEnd.Location;
			}
		}

		UNavigationPath* Path = (bStartProjected && bEndProjected)
			? UNavigationSystemV1::FindPathToLocationSynchronously(World, NavStart, NavEnd)
			: nullptr;
		const bool bPathFound = Path && Path->IsValid();
		const bool bPartial = Path ? Path->IsPartial() : false;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("requestedStart"), MakeIntVecArray(Start));
		Result->SetArrayField(TEXT("requestedEnd"), MakeIntVecArray(End));
		Result->SetBoolField(TEXT("startProjected"), bStartProjected);
		Result->SetBoolField(TEXT("endProjected"), bEndProjected);
		Result->SetArrayField(TEXT("navStart"), MakeIntVecArray(NavStart));
		Result->SetArrayField(TEXT("navEnd"), MakeIntVecArray(NavEnd));
		Result->SetBoolField(TEXT("pathFound"), bPathFound);
		Result->SetBoolField(TEXT("partial"), bPartial);
		Result->SetBoolField(TEXT("reachable"), bPathFound && !bPartial);
		if (Path)
		{
			Result->SetNumberField(TEXT("length"), Path->GetPathLength());
			if (bIncludePathPoints)
			{
				TArray<TSharedPtr<FJsonValue>> Points;
				for (const FVector& Point : Path->PathPoints)
				{
					TSharedRef<FJsonObject> PointObj = MakeShared<FJsonObject>();
					PointObj->SetArrayField(TEXT("location"), MakeIntVecArray(Point));
					Points.Add(MakeShared<FJsonValueObject>(PointObj));
				}
				Result->SetNumberField(TEXT("pointCount"), Points.Num());
				Result->SetArrayField(TEXT("points"), Points);
			}
		}
		return SuccessJson(Result);
	}

	FString RaycastImpl(UWorld* World, const FVector& Start, const FVector& End)
	{
		FHitResult Hit;
		FCollisionQueryParams Params(FName(TEXT("MCPRaycast")), /*bTraceComplex=*/true);
		const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("start"), MakeIntVecArray(Start));
		Result->SetArrayField(TEXT("end"), MakeIntVecArray(End));
		Result->SetBoolField(TEXT("hit"), bHit);
		if (bHit)
		{
			if (const AActor* HitActor = Hit.GetActor())
			{
				Result->SetStringField(TEXT("actor"), HitActor->GetActorLabel());
				Result->SetStringField(TEXT("actorName"), HitActor->GetName());
				Result->SetStringField(TEXT("class"), HitActor->GetClass()->GetName());
				Result->SetStringField(TEXT("category"), ClassifyActor(HitActor));
			}
			if (const UPrimitiveComponent* HitComp = Hit.GetComponent())
			{
				Result->SetStringField(TEXT("component"), HitComp->GetName());
			}
			Result->SetArrayField(TEXT("impactPoint"), MakeIntVecArray(Hit.ImpactPoint));
			Result->SetArrayField(TEXT("impactNormal"), MakeIntVecArray(Hit.ImpactNormal));
			Result->SetNumberField(TEXT("distance"), FMath::RoundToInt(Hit.Distance));
		}
		return SuccessJson(Result);
	}

	FString Raycast(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FVector Start;
		if (!TryGetVec(Args, TEXT("start"), Start))
		{
			return ErrorJson(TEXT("Missing required 'start' [x,y,z]."));
		}

		FVector End;
		if (!TryGetVec(Args, TEXT("end"), End))
		{
			FVector Direction;
			if (!TryGetVec(Args, TEXT("direction"), Direction) || Direction.IsNearlyZero())
			{
				return ErrorJson(TEXT("Provide 'end' [x,y,z] or a non-zero 'direction' [x,y,z]."));
			}
			double Length = 100000.0;
			Args->TryGetNumberField(TEXT("length"), Length);
			End = Start + Direction.GetSafeNormal() * Length;
		}

		return RaycastImpl(World, Start, End);
	}

	FString WhatIsUnderCamera(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World || !GEditor)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FLevelEditorViewportClient* Best = nullptr;
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective())
			{
				Best = Client;
				if (Client->Viewport == GEditor->GetActiveViewport())
				{
					break;
				}
			}
		}
		if (!Best)
		{
			return ErrorJson(TEXT("No perspective level viewport is available."));
		}

		const FVector CamLoc = Best->GetViewLocation();
		const FRotator CamRot = Best->GetViewRotation();
		double Length = 10000000.0;
		Args->TryGetNumberField(TEXT("length"), Length);
		const FVector End = CamLoc + CamRot.Vector() * Length;

		FString Inner = RaycastImpl(World, CamLoc, End);
		TSharedPtr<FJsonObject> InnerObj = ParseJsonObject(Inner);
		if (!InnerObj.IsValid())
		{
			return Inner;
		}
		InnerObj->SetArrayField(TEXT("cameraLocation"), MakeIntVecArray(CamLoc));
		TSharedRef<FJsonObject> CamRotObj = MakeShared<FJsonObject>();
		CamRotObj->SetNumberField(TEXT("pitch"), CamRot.Pitch);
		CamRotObj->SetNumberField(TEXT("yaw"), CamRot.Yaw);
		CamRotObj->SetNumberField(TEXT("roll"), CamRot.Roll);
		InnerObj->SetObjectField(TEXT("cameraRotation"), CamRotObj);
		return SuccessJson(InnerObj.ToSharedRef());
	}

	// view -> (pitch,yaw,roll) for the ortho camera. Top looks straight down with world +X up
	// and +Y right; front/side are the standard orthographic elevations with world +Z up.
	FRotator ViewRotation(const FString& View)
	{
		if (View.Equals(TEXT("front"), ESearchCase::IgnoreCase))
		{
			return FRotator(0.0, 0.0, 0.0);
		}
		if (View.Equals(TEXT("side"), ESearchCase::IgnoreCase))
		{
			return FRotator(0.0, 90.0, 0.0);
		}
		return FRotator(-90.0, 0.0, 0.0); // top
	}

	void DrawRectOutline(TArray<FColor>& Pixels, int32 W, int32 H, int32 X0, int32 Y0, int32 X1, int32 Y1, const FColor& Color)
	{
		X0 = FMath::Clamp(X0, 0, W - 1);
		X1 = FMath::Clamp(X1, 0, W - 1);
		Y0 = FMath::Clamp(Y0, 0, H - 1);
		Y1 = FMath::Clamp(Y1, 0, H - 1);
		for (int32 X = X0; X <= X1; ++X)
		{
			Pixels[Y0 * W + X] = Color;
			Pixels[Y1 * W + X] = Color;
		}
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			Pixels[Y * W + X0] = Color;
			Pixels[Y * W + X1] = Color;
		}
	}

	FString CaptureSceneMap(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		double ResNumber = 1024.0;
		Args->TryGetNumberField(TEXT("resolution"), ResNumber);
		const int32 Size = FMath::Clamp(static_cast<int32>(ResNumber), 256, 4096);

		FString View = TEXT("top");
		Args->TryGetStringField(TEXT("view"), View);

		double PaddingPercent = 10.0;
		Args->TryGetNumberField(TEXT("paddingPercent"), PaddingPercent);
		const double Pad = 1.0 + FMath::Clamp(PaddingPercent, 0.0, 200.0) / 100.0;

		bool bOverlay = true;
		Args->TryGetBoolField(TEXT("drawRegions"), bOverlay);

		int32 Considered = 0;
		FBox Bounds = ComputeLevelBounds(World, Considered);
		// Allow an explicit framing override.
		FVector OverrideCenter, OverrideExtent;
		if (TryGetVec(Args, TEXT("center"), OverrideCenter) && TryGetVec(Args, TEXT("extent"), OverrideExtent))
		{
			Bounds = FBox(OverrideCenter - OverrideExtent, OverrideCenter + OverrideExtent);
		}
		if (!Bounds.IsValid)
		{
			return ErrorJson(TEXT("Scene has no spatially-meaningful actors to frame."));
		}

		const FVector Center = Bounds.GetCenter();
		const FVector BSize = Bounds.GetSize();
		const FRotator CamRot = ViewRotation(View);
		const FMatrix RotMat = FRotationMatrix(CamRot);
		const FVector Fwd = RotMat.GetScaledAxis(EAxis::X);
		const FVector Right = RotMat.GetScaledAxis(EAxis::Y);
		const FVector Up = RotMat.GetScaledAxis(EAxis::Z);

		// In-plane extents projected onto the camera's right/up axes (depth axis ignored for ortho).
		const double ExtentRight = FMath::Abs(BSize.X * Right.X) + FMath::Abs(BSize.Y * Right.Y) + FMath::Abs(BSize.Z * Right.Z);
		const double ExtentUp = FMath::Abs(BSize.X * Up.X) + FMath::Abs(BSize.Y * Up.Y) + FMath::Abs(BSize.Z * Up.Z);
		const double OrthoWidth = FMath::Max3(ExtentRight, ExtentUp, 100.0) * Pad;

		const double PushBack = BSize.Size() + 100000.0;
		const FVector CamLoc = Center - Fwd * PushBack;

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RT->RenderTargetFormat = RTF_RGBA8;
		RT->ClearColor = FLinearColor::Black;
		RT->InitAutoFormat(Size, Size);
		RT->TargetGamma = 2.2f;
		RT->UpdateResourceImmediate(true);

		const FName CaptureName = MakeUniqueObjectName(GetTransientPackage(), USceneCaptureComponent2D::StaticClass(), TEXT("MCPSceneMapCapture"));
		USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), CaptureName, RF_Transient);
		Capture->TextureTarget = RT;
		Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->ShowFlags = FEngineShowFlags(ESFIM_Game);
		Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		Capture->OrthoWidth = OrthoWidth;
		Capture->RegisterComponentWithWorld(World);
		Capture->SetWorldLocationAndRotation(CamLoc, CamRot);
		Capture->CaptureScene();

		TArray<FColor> Bitmap;
		bool bRead = false;
		if (FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource())
		{
			bRead = RTResource->ReadPixels(Bitmap);
		}

		// Tear down the transient capture rig before we do anything else.
		Capture->UnregisterComponent();
		Capture->DestroyComponent();

		if (!bRead || Bitmap.Num() < Size * Size)
		{
			return ErrorJson(TEXT("Failed to read scene-capture pixels."));
		}
		for (FColor& Pixel : Bitmap)
		{
			Pixel.A = 255;
		}

		// World -> pixel using the camera's right/up axes (screen up = +Up -> smaller pixelY).
		const double WorldPerPixel = OrthoWidth / static_cast<double>(Size);
		auto WorldToPixel = [&](const FVector& P, int32& OutX, int32& OutY)
		{
			const FVector D = P - Center;
			const double SX = FVector::DotProduct(D, Right);
			const double SY = FVector::DotProduct(D, Up);
			OutX = FMath::RoundToInt(Size * 0.5 + SX / WorldPerPixel);
			OutY = FMath::RoundToInt(Size * 0.5 - SY / WorldPerPixel);
		};

		// Legend: one entry per dense region, keyed to its pixel position so the agent can map
		// labels onto the image. Optionally outline each region on the bitmap itself.
		const TArray<TSharedPtr<FJsonValue>> Regions = BuildRegionSummaries(World, Bounds, 4, 12);
		TArray<TSharedPtr<FJsonValue>> Legend;
		for (const TSharedPtr<FJsonValue>& RegionVal : Regions)
		{
			const TSharedPtr<FJsonObject> Region = RegionVal->AsObject();
			if (!Region.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* CenterArr = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* SizeArr = nullptr;
			if (!Region->TryGetArrayField(TEXT("center"), CenterArr) || !CenterArr || CenterArr->Num() < 3)
			{
				continue;
			}
			const FVector RC((*CenterArr)[0]->AsNumber(), (*CenterArr)[1]->AsNumber(), (*CenterArr)[2]->AsNumber());
			int32 Px = 0, Py = 0;
			WorldToPixel(RC, Px, Py);

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("label"), Region->GetStringField(TEXT("dominantCategory")));
			Entry->SetNumberField(TEXT("actorCount"), Region->GetIntegerField(TEXT("actorCount")));
			Entry->SetArrayField(TEXT("world"), MakeIntVecArray(RC));
			TArray<TSharedPtr<FJsonValue>> PixelArr;
			PixelArr.Add(MakeShared<FJsonValueNumber>(Px));
			PixelArr.Add(MakeShared<FJsonValueNumber>(Py));
			Entry->SetArrayField(TEXT("pixel"), PixelArr);
			Legend.Add(MakeShared<FJsonValueObject>(Entry));

			if (bOverlay && Region->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr && SizeArr->Num() >= 3)
			{
				const FVector HalfSize(FMath::Abs((*SizeArr)[0]->AsNumber()) * 0.5, FMath::Abs((*SizeArr)[1]->AsNumber()) * 0.5, FMath::Abs((*SizeArr)[2]->AsNumber()) * 0.5);
				int32 Ax, Ay, Bx, By;
				WorldToPixel(RC - HalfSize, Ax, Ay);
				WorldToPixel(RC + HalfSize, Bx, By);
				DrawRectOutline(Bitmap, Size, Size, FMath::Min(Ax, Bx), FMath::Min(Ay, By), FMath::Max(Ax, Bx), FMath::Max(Ay, By), FColor(255, 80, 80, 255));
			}
		}

		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size, Size, Bitmap, Png);

		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("screenshots"));
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString FileName = FString::Printf(TEXT("scenemap_%s_%s.png"), *View.ToLower(), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
		FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, FileName));
		FPaths::MakePlatformFilename(FullPath);
		FFileHelper::SaveArrayToFile(Png, *FullPath);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("view"), View);
		Result->SetStringField(TEXT("path"), FullPath);
		Result->SetNumberField(TEXT("width"), Size);
		Result->SetNumberField(TEXT("height"), Size);
		Result->SetNumberField(TEXT("actorsFramed"), Considered);
		Result->SetObjectField(TEXT("coveredBounds"), BoxToJson(Bounds));
		Result->SetNumberField(TEXT("worldUnitsPerPixel"), WorldPerPixel);
		Result->SetStringField(TEXT("orientation"),
			View.Equals(TEXT("top"), ESearchCase::IgnoreCase)
				? TEXT("Top-down orthographic. World +X is up (north), +Y is right (east). pixel [x,y] maps from world via the legend.")
				: TEXT("Orthographic elevation. World +Z is up. pixel [x,y] maps from world via the legend."));
		Result->SetArrayField(TEXT("legend"), Legend);

		bool bInline = true;
		Args->TryGetBoolField(TEXT("inline"), bInline);
		if (bInline)
		{
			TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
			Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			Image->SetStringField(TEXT("data"), FBase64::Encode(Png.GetData(), static_cast<uint32>(Png.Num())));
			Result->SetObjectField(TEXT("_imageContent"), Image);
		}
		return SuccessJson(Result);
	}

	UStaticMesh* ResolveStaticMeshPath(const FString& Path)
	{
		FString P = Path;
		P.TrimStartAndEndInline();
		if (P.IsEmpty()) { return nullptr; }
		UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, *P);
		if (!M && !P.Contains(TEXT(".")))
		{
			int32 Slash = INDEX_NONE;
			if (P.FindLastChar(TCHAR('/'), Slash))
			{
				M = LoadObject<UStaticMesh>(nullptr, *(P + TEXT(".") + P.RightChop(Slash + 1)));
			}
		}
		return M;
	}

	// Vertical down-ray vs the world AABB of every given instanced-mesh instance. ISMs (e.g.
	// PCG-generated) usually have no collision so a LineTrace misses them; this is the opt-in path
	// that lets place_meshes rest on top of them. Bounds-level, not exact geometry: returns the
	// highest instance top-face within [ZBot, ZTop] under (X,Y) — enough to sit an actor on top.
	bool TraceInstancesDown(const TArray<UInstancedStaticMeshComponent*>& Comps,
		double X, double Y, double ZTop, double ZBot, double& OutZ, FString& OutRestingOn)
	{
		bool bFound = false;
		double BestZ = TNumericLimits<double>::Lowest();
		for (UInstancedStaticMeshComponent* C : Comps)
		{
			if (!IsValid(C)) { continue; }
			UStaticMesh* M = C->GetStaticMesh();
			if (!M) { continue; }
			const FBox CompBox = C->Bounds.GetBox();
			if (X < CompBox.Min.X || X > CompBox.Max.X || Y < CompBox.Min.Y || Y > CompBox.Max.Y) { continue; }
			const FBoxSphereBounds LocalBounds = M->GetBounds();
			const int32 Count = C->GetInstanceCount();
			for (int32 i = 0; i < Count; ++i)
			{
				FTransform T;
				if (!C->GetInstanceTransform(i, T, /*bWorldSpace*/ true)) { continue; }
				const FBox WB = LocalBounds.TransformBy(T).GetBox();
				if (X < WB.Min.X || X > WB.Max.X || Y < WB.Min.Y || Y > WB.Max.Y) { continue; }
				const double TopZ = WB.Max.Z;
				if (TopZ < ZBot || TopZ > ZTop) { continue; }
				if (TopZ > BestZ)
				{
					BestZ = TopZ;
					OutRestingOn = C->GetOwner() ? C->GetOwner()->GetActorLabel() : C->GetName();
					bFound = true;
				}
			}
		}
		if (bFound) { OutZ = BestZ; }
		return bFound;
	}

	// Spawn StaticMesh actors and (by default) snap each to the surface directly below its location:
	// trace down, drop the mesh so its bbox bottom sits flush on the hit (no float/sink, no pivot-offset
	// math by the agent), optionally tilt to the surface normal. Batch + one undo. This fuses the
	// raycast perception with placement so the agent states intent, not hand-computed coordinates.
	// Note: by default traces collision geometry, so it lands on terrain/landscape and collidable
	// meshes; non-collidable instances (e.g. PCG-generated ISMs) are skipped UNLESS includeInstances
	// is set, which additionally rests on the top of the nearest instanced-mesh bounds below.
	// ---------------------------------------------------------------------------------------------
	// Deterministic geometric critic — the fact-based half of scene verification.
	//
	// This is the part of the verify loop that does NOT need an LLM: floating / buried / over-void /
	// overlap (and out-of-bounds when a review region is supplied) are all decidable from traces and
	// AABB math. Because it is deterministic it lives in C++ and rides back inside the generation
	// tool's own result (place_meshes) so the geometric truth can't be skipped, and is also exposed
	// on demand as verify_scene. The SUBJECTIVE half (aesthetics) still needs vision and stays
	// agent-side — this critic deliberately makes no aesthetic claim.
	// ---------------------------------------------------------------------------------------------
	struct FGeoCriticConfig
	{
		double FloatCm   = 50.0;    // bottom this far ABOVE the nearest surface => floating
		double BuryCm    = 50.0;    // bottom this far BELOW the nearest surface => buried
		double OverlapCm = 25.0;    // min-axis interpenetration before a pair counts as overlapping
		int32  MaxActors = 600;     // bound the work; only the first N are evaluated (logged via checkedActors)
		int32  MaxIssues = 60;      // cap the reported issue list (counts stay complete)
		bool   bHasRegion = false;
		FBox   Region = FBox(ForceInit);
	};

	// Evaluate Actors and return a verdict JSON. Read-only to the scene. Also yields pass / issueCount /
	// one-line summary via out-params so callers can feed them straight into RecordGeometricVerdict.
	TSharedRef<FJsonObject> RunGeometricCritic(UWorld* World, const TArray<AActor*>& InActors,
		const FGeoCriticConfig& Cfg, bool& OutPass, int32& OutIssueCount, FString& OutSummary)
	{
		struct FItem { AActor* Actor = nullptr; FBox Bounds = FBox(ForceInit); };
		TArray<FItem> Items;
		for (AActor* A : InActors)
		{
			if (!IsValid(A)) { continue; }
			const FBox B = GetActorWorldBounds(A);
			if (!B.IsValid) { continue; }
			FItem I; I.Actor = A; I.Bounds = B;
			Items.Add(I);
			if (Items.Num() >= Cfg.MaxActors) { break; }
		}

		int32 NFloat = 0, NBury = 0, NOverlap = 0, NOob = 0, NVoid = 0;
		TArray<TSharedPtr<FJsonValue>> Issues;
		auto AddIssue = [&Issues, &Cfg](const TSharedRef<FJsonObject>& O)
		{
			if (Issues.Num() < Cfg.MaxIssues) { Issues.Add(MakeShared<FJsonValueObject>(O)); }
		};

		// floating / buried / over-void — one down-trace per actor, ignoring the actor itself so we
		// measure the surface BELOW it, not its own collision.
		for (const FItem& It : Items)
		{
			const FVector C = It.Bounds.GetCenter();
			const double BottomZ = It.Bounds.Min.Z;
			const double TopZ = It.Bounds.Max.Z;

			FHitResult Hit;
			FCollisionQueryParams P(FName(TEXT("MCPGeoCritic")), /*bTraceComplex=*/true);
			P.AddIgnoredActor(It.Actor);
			const FVector Start(C.X, C.Y, TopZ + 50.0);
			const FVector End(C.X, C.Y, BottomZ - 100000.0);
			if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, P))
			{
				const double Gap = BottomZ - Hit.ImpactPoint.Z; // +above surface / -below surface
				if (Gap > Cfg.FloatCm)
				{
					++NFloat;
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("type"), TEXT("floating"));
					O->SetStringField(TEXT("actor"), It.Actor->GetActorLabel());
					O->SetNumberField(TEXT("gapCm"), FMath::RoundToInt(Gap));
					if (const AActor* GA = Hit.GetActor()) { O->SetStringField(TEXT("over"), GA->GetActorLabel()); }
					AddIssue(O);
				}
				else if (Gap < -Cfg.BuryCm)
				{
					++NBury;
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("type"), TEXT("buried"));
					O->SetStringField(TEXT("actor"), It.Actor->GetActorLabel());
					O->SetNumberField(TEXT("depthCm"), FMath::RoundToInt(-Gap));
					AddIssue(O);
				}
				// else: bottom is within [-BuryCm, +FloatCm] of the surface => resting => ok.
			}
			else
			{
				++NVoid;
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("void"));
				O->SetStringField(TEXT("actor"), It.Actor->GetActorLabel());
				O->SetStringField(TEXT("detail"), TEXT("no surface beneath the object"));
				AddIssue(O);
			}
		}

		// overlap — mutual AABB interpenetration beyond tolerance. Deterministic and collision-setup
		// independent (flush-stacked or merely-touching boxes share a face => ~0 overlap => not flagged).
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			for (int32 j = i + 1; j < Items.Num(); ++j)
			{
				const FBox& A = Items[i].Bounds;
				const FBox& Bx = Items[j].Bounds;
				if (!A.Intersect(Bx)) { continue; }
				const FBox Ov = A.Overlap(Bx);
				if (!Ov.IsValid) { continue; }
				const FVector S = Ov.GetSize();
				const double MinPen = FMath::Min3(S.X, S.Y, S.Z);
				if (MinPen > Cfg.OverlapCm)
				{
					++NOverlap;
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("type"), TEXT("overlap"));
					O->SetStringField(TEXT("actor"), Items[i].Actor->GetActorLabel());
					O->SetStringField(TEXT("with"), Items[j].Actor->GetActorLabel());
					O->SetNumberField(TEXT("penetrationCm"), FMath::RoundToInt(MinPen));
					AddIssue(O);
				}
			}
		}

		// out-of-bounds — only meaningful when a review region is supplied (the inline place_meshes
		// path has none, so it skips this check honestly rather than inventing a boundary).
		if (Cfg.bHasRegion && Cfg.Region.IsValid)
		{
			for (const FItem& It : Items)
			{
				if (!Cfg.Region.IsInsideOrOn(It.Bounds.GetCenter()))
				{
					++NOob;
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("type"), TEXT("outOfBounds"));
					O->SetStringField(TEXT("actor"), It.Actor->GetActorLabel());
					O->SetArrayField(TEXT("center"), MakeIntVecArray(It.Bounds.GetCenter()));
					AddIssue(O);
				}
			}
		}

		const int32 Total = NFloat + NBury + NOverlap + NOob + NVoid;
		OutPass = (Total == 0);
		OutIssueCount = Total;

		TArray<FString> Parts;
		if (NOverlap) { Parts.Add(FString::Printf(TEXT("%d overlap"), NOverlap)); }
		if (NFloat)   { Parts.Add(FString::Printf(TEXT("%d floating"), NFloat)); }
		if (NBury)    { Parts.Add(FString::Printf(TEXT("%d buried"), NBury)); }
		if (NVoid)    { Parts.Add(FString::Printf(TEXT("%d over-void"), NVoid)); }
		if (NOob)     { Parts.Add(FString::Printf(TEXT("%d out-of-bounds"), NOob)); }
		OutSummary = (Total == 0)
			? FString::Printf(TEXT("%d actor(s) checked, no geometric issues"), Items.Num())
			: FString::Printf(TEXT("%d geometric issue(s): %s"), Total, *FString::Join(Parts, TEXT(", ")));

		TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
		V->SetBoolField(TEXT("pass"), OutPass);
		V->SetNumberField(TEXT("issueCount"), Total);
		V->SetStringField(TEXT("summary"), OutSummary);
		V->SetNumberField(TEXT("checkedActors"), Items.Num());
		TSharedRef<FJsonObject> ByType = MakeShared<FJsonObject>();
		ByType->SetNumberField(TEXT("overlap"), NOverlap);
		ByType->SetNumberField(TEXT("floating"), NFloat);
		ByType->SetNumberField(TEXT("buried"), NBury);
		ByType->SetNumberField(TEXT("overVoid"), NVoid);
		ByType->SetNumberField(TEXT("outOfBounds"), NOob);
		V->SetObjectField(TEXT("byType"), ByType);
		V->SetBoolField(TEXT("truncated"), Total > Issues.Num());
		V->SetArrayField(TEXT("issues"), Issues);
		return V;
	}

	FString PlaceMeshes(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World) { return ErrorJson(TEXT("Editor world is not available.")); }

		// Scene-generation gate: placing meshes requires an active scene brief for this level.
		{
			FString GateReason;
			if (!::WorldDataMCP::HasActiveSceneBrief(World->GetMapName(), GateReason))
			{
				return ErrorJson(GateReason);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Args->TryGetArrayField(TEXT("meshes"), Items) || !Items || Items->Num() == 0)
		{
			return ErrorJson(TEXT("Missing 'meshes' (array of placement items: {mesh, location, ...})."));
		}

		bool bSnapDefault = true;
		Args->TryGetBoolField(TEXT("snapToSurface"), bSnapDefault);
		bool bAlignDefault = false;
		Args->TryGetBoolField(TEXT("alignToNormal"), bAlignDefault);
		bool bInclDefault = false;
		Args->TryGetBoolField(TEXT("includeInstances"), bInclDefault);
		double TraceUp = 100000.0;
		Args->TryGetNumberField(TEXT("traceUp"), TraceUp);
		double TraceDown = 100000.0;
		Args->TryGetNumberField(TEXT("traceDown"), TraceDown);
		FString Folder;
		Args->TryGetStringField(TEXT("folder"), Folder);

		const int32 MaxItems = 1000;
		FScopedTransaction Transaction(FText::FromString(TEXT("Place Meshes")));

		// Instanced-mesh components are gathered lazily — only when an item opts into includeInstances —
		// so the default collision-only path keeps its exact behaviour and cost.
		TArray<UInstancedStaticMeshComponent*> InstComps;
		bool bInstGathered = false;
		auto EnsureInstComps = [&]()
		{
			if (bInstGathered) { return; }
			bInstGathered = true;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				if (!IsValid(A)) { continue; }
				TArray<UInstancedStaticMeshComponent*> CC;
				A->GetComponents<UInstancedStaticMeshComponent>(CC, /*bIncludeFromChildActors*/ true);
				InstComps.Append(CC);
			}
		};

		TArray<TSharedPtr<FJsonValue>> Placed, Failed;
		TArray<AActor*> PlacedActors;   // kept so the inline geometric critic can review this batch
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			if (Placed.Num() >= MaxItems) { break; }
			const TSharedPtr<FJsonObject> Item = V.IsValid() ? V->AsObject() : nullptr;
			if (!Item.IsValid()) { continue; }

			FString MeshPath;
			Item->TryGetStringField(TEXT("mesh"), MeshPath);
			UStaticMesh* Mesh = ResolveStaticMeshPath(MeshPath);
			if (!Mesh)
			{
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("mesh"), MeshPath);
				F->SetStringField(TEXT("error"), TEXT("StaticMesh not found"));
				Failed.Add(MakeShared<FJsonValueObject>(F));
				continue;
			}

			FVector Loc = FVector::ZeroVector;
			TryGetVec(Item, TEXT("location"), Loc);
			FRotator Rot = FRotator::ZeroRotator;
			TryGetRot(Item, TEXT("rotation"), Rot);
			FVector Scale(1.0, 1.0, 1.0);
			{
				double S = 0.0;
				FVector SV;
				if (Item->TryGetNumberField(TEXT("scale"), S) && S != 0.0) { Scale = FVector(S); }
				else if (TryGetVec(Item, TEXT("scale"), SV)) { Scale = SV; }
			}
			bool bSnap = bSnapDefault;
			Item->TryGetBoolField(TEXT("snapToSurface"), bSnap);
			bool bAlign = bAlignDefault;
			Item->TryGetBoolField(TEXT("alignToNormal"), bAlign);
			bool bIncl = bInclDefault;
			Item->TryGetBoolField(TEXT("includeInstances"), bIncl);

			bool bSnapped = false;
			FVector GroundPt = Loc;
			FVector GroundNormal = FVector::UpVector;
			FString RestingOn;
			if (bSnap)
			{
				FHitResult Hit;
				FCollisionQueryParams P(FName(TEXT("MCPPlaceMeshes")), /*bTraceComplex=*/true);
				const FVector Start(Loc.X, Loc.Y, Loc.Z + TraceUp);
				const FVector End(Loc.X, Loc.Y, Loc.Z - TraceDown);
				if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, P))
				{
					GroundPt = Hit.ImpactPoint;
					GroundNormal = Hit.ImpactNormal.GetSafeNormal();
					bSnapped = true;
					if (const AActor* HA = Hit.GetActor()) { RestingOn = HA->GetActorLabel(); }
				}
				// Optionally also rest on non-collidable instanced meshes (e.g. PCG ISMs) the line
				// trace cannot hit. Take whichever surface is higher — the first one hit going down.
				if (bIncl)
				{
					EnsureInstComps();
					double InstZ = 0.0;
					FString InstOn;
					if (TraceInstancesDown(InstComps, Loc.X, Loc.Y, Start.Z, End.Z, InstZ, InstOn))
					{
						if (!bSnapped || InstZ > GroundPt.Z)
						{
							GroundPt = FVector(Loc.X, Loc.Y, InstZ);
							GroundNormal = FVector::UpVector;
							bSnapped = true;
							RestingOn = InstOn;
						}
					}
				}
			}

			FQuat FinalQ = Rot.Quaternion();
			if (bSnap && bAlign && bSnapped)
			{
				FinalQ = FQuat::FindBetweenNormals(FVector::UpVector, GroundNormal) * FinalQ;
			}

			const FBoxSphereBounds B = Mesh->GetBounds();
			FVector FinalLoc = Loc;
			if (bSnapped)
			{
				const double BottomOffset = (B.Origin.Z - B.BoxExtent.Z) * Scale.Z; // local lowest point rel. to pivot, scaled
				FinalLoc = FVector(GroundPt.X, GroundPt.Y, GroundPt.Z - BottomOffset);
			}

			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FinalQ, FinalLoc, Scale));
			if (!Actor)
			{
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				F->SetStringField(TEXT("error"), TEXT("SpawnActor failed"));
				Failed.Add(MakeShared<FJsonValueObject>(F));
				continue;
			}
			if (UStaticMeshComponent* SMC = Actor->GetStaticMeshComponent()) { SMC->SetStaticMesh(Mesh); }
			FString Label;
			if (Item->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty()) { Actor->SetActorLabel(Label); }
			if (!Folder.IsEmpty()) { Actor->SetFolderPath(FName(*Folder)); }
			PlacedActors.Add(Actor);

			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("actor"), Actor->GetActorLabel());
			R->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			R->SetArrayField(TEXT("location"), MakeIntVecArray(FinalLoc));
			R->SetBoolField(TEXT("snapped"), bSnapped);
			if (bSnap && !bSnapped) { R->SetBoolField(TEXT("surfaceFound"), false); }
			if (!RestingOn.IsEmpty()) { R->SetStringField(TEXT("restingOn"), RestingOn); }
			Placed.Add(MakeShared<FJsonValueObject>(R));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("placedCount"), Placed.Num());
		Result->SetArrayField(TEXT("placed"), Placed);
		if (Failed.Num() > 0) { Result->SetArrayField(TEXT("failed"), Failed); }

		// Verification gate, part 1 (unskippable verdict): this generation leaves the scene needing a
		// whole-scene verify_scene, and the deterministic geometric critic for THIS batch rides back
		// inline so the agent receives the geometric truth in the same payload — it cannot place and
		// then eyeball-declare success. MarkSceneDirty is deliberately NOT cleared here; only a
		// whole-scene verify_scene resolves the gate (one clean batch must not unlock a junk scene).
		::WorldDataMCP::MarkSceneDirty(World->GetMapName());
		if (PlacedActors.Num() > 0)
		{
			FGeoCriticConfig Cfg;   // batch review: no region (out-of-bounds needs an explicit boundary)
			bool bPass = true; int32 IssueCount = 0; FString Summary;
			TSharedRef<FJsonObject> Verdict = RunGeometricCritic(World, PlacedActors, Cfg, bPass, IssueCount, Summary);
			Verdict->SetStringField(TEXT("scope"), TEXT("thisBatch"));
			Verdict->SetStringField(TEXT("note"), bPass
				? TEXT("This batch is geometrically clean, but the SCENE is still unverified. Run verify_scene (whole-scene) before saving; aesthetics are not judged here.")
				: TEXT("This batch has geometric issues — fix them, then run verify_scene over the whole scene. Do NOT declare the scene done off a screenshot."));
			Result->SetObjectField(TEXT("geometricVerdict"), Verdict);
		}

		return SuccessJson(Result);
	}

	// verify_scene — the server-side, skill-independent half of the verify loop. Runs the
	// deterministic geometric critic over the whole scene (or a region) and RECORDS the authoritative
	// verdict the save gate trusts. This is what makes "must verify" survive: it's a tool BOTH the
	// desktop MCP clients and the in-editor WorldData Agent can call, instead of a skill only one of them
	// can load. Aesthetics are deliberately left to the agent (they need vision).
	FString VerifyScene(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World) { return ErrorJson(TEXT("Editor world is not available.")); }

		const FString LevelName = World->GetMapName();

		// Optional review region (also enables the out-of-bounds check + scopes the gather).
		FGeoCriticConfig Cfg;
		FVector Center, Extent, Min, Max;
		if (TryGetVec(Args, TEXT("center"), Center) && TryGetVec(Args, TEXT("extent"), Extent))
		{
			Cfg.bHasRegion = true;
			Cfg.Region = FBox(Center - Extent, Center + Extent);
		}
		else if (TryGetVec(Args, TEXT("min"), Min) && TryGetVec(Args, TEXT("max"), Max))
		{
			Cfg.bHasRegion = true;
			Cfg.Region = FBox(Min, Max);
		}
		Args->TryGetNumberField(TEXT("floatThreshold"), Cfg.FloatCm);
		Args->TryGetNumberField(TEXT("buryThreshold"), Cfg.BuryCm);
		Args->TryGetNumberField(TEXT("overlapTolerance"), Cfg.OverlapCm);

		// Generated placed content = StaticMesh actors (place_meshes / place_along_axis / spawn_actor-
		// with-mesh). Skip world-spanning things (landscape) and, with a region, anything outside it.
		// NOTE v1: ISM/foliage/PCG instance routes are not yet gathered per-instance — follow-up.
		TArray<AActor*> Actors;
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			AStaticMeshActor* A = *It;
			if (!IsValid(A)) { continue; }
			const FBox B = GetActorWorldBounds(A);
			if (!IsSpatiallyMeaningfulActor(A, B)) { continue; }
			if (Cfg.bHasRegion && !Cfg.Region.Intersect(B)) { continue; }
			Actors.Add(A);
		}

		bool bPass = true; int32 IssueCount = 0; FString Summary;
		TSharedRef<FJsonObject> Verdict = RunGeometricCritic(World, Actors, Cfg, bPass, IssueCount, Summary);
		Verdict->SetStringField(TEXT("scope"), Cfg.bHasRegion ? TEXT("region") : TEXT("wholeScene"));

		// Authoritative whole-scene verdict — clears the dirty flag and is what the save gate trusts.
		::WorldDataMCP::RecordGeometricVerdict(LevelName, bPass, IssueCount, Summary);

		const ::WorldDataMCP::FSceneBrief Brief = ::WorldDataMCP::GetActiveSceneBrief();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("verdict"), bPass ? TEXT("GEOMETRIC_PASS") : TEXT("GEOMETRIC_FAIL"));
		Result->SetBoolField(TEXT("geometricPass"), bPass);
		Result->SetObjectField(TEXT("geometric"), Verdict);
		if (Brief.bValid) { Result->SetStringField(TEXT("concept"), Brief.OneIdea); }
		Result->SetStringField(TEXT("aestheticNote"), bPass
			? TEXT("Geometry passes. Aesthetics are NOT judged server-side: now capture_scene_map (top-down) + an eye-level capture_viewport hero view and score concept-legibility / composition / hierarchy / layering / lighting against the brief (design-playbook 0-30). Only then is the scene done.")
			: TEXT("Geometry FAILS — refine the listed issues (see geometric.issues) and re-run verify_scene before any aesthetic pass; a geometrically broken scene cannot pass."));
		Result->SetStringField(TEXT("saveGate"), bPass
			? TEXT("save is unblocked for this scene (geometric half).")
			: TEXT("save_current_level / save_all_dirty will refuse until this passes, unless confirmUnverified:true."));
		return SuccessJson(Result);
	}

	// Formal-layout primitive: place meshes at regular intervals along an axis (allée / hedge row).
	// Optional paired rows mirror across the axis perpendicular; optional hero terminates the axis.
	FString PlaceAlongAxis(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World) { return ErrorJson(TEXT("Editor world is not available.")); }

		::WorldDataMCP::FSceneBriefRouting BriefRouting;
		{
			FString GateReason;
			if (!::WorldDataMCP::HasActiveSceneBrief(World->GetMapName(), GateReason))
			{
				return ErrorJson(GateReason);
			}
			BriefRouting = ::WorldDataMCP::BuildBriefRouting(::WorldDataMCP::GetActiveSceneBrief());
		}

		FString MeshPath;
		Args->TryGetStringField(TEXT("mesh"), MeshPath);
		UStaticMesh* Mesh = ResolveStaticMeshPath(MeshPath);
		if (!Mesh)
		{
			return ErrorJson(TEXT("Missing or invalid 'mesh' (StaticMesh asset path)."));
		}

		FVector Origin = FVector::ZeroVector;
		TryGetVec(Args, TEXT("origin"), Origin);
		FVector Direction = FVector(1.0, 0.0, 0.0);
		TryGetVec(Args, TEXT("direction"), Direction);
		Direction = Direction.GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			return ErrorJson(TEXT("'direction' must be a non-zero vector."));
		}

		double Spacing = 400.0;
		Args->TryGetNumberField(TEXT("spacing"), Spacing);
		if (Spacing <= 0.0)
		{
			return ErrorJson(TEXT("'spacing' must be positive."));
		}

		int32 SlotCount = 5;
		{
			double Num = SlotCount;
			if (Args->TryGetNumberField(TEXT("slotCount"), Num))
			{
				SlotCount = FMath::Clamp(static_cast<int32>(Num), 1, 500);
			}
		}

		bool bPairedRows = false;
		Args->TryGetBoolField(TEXT("pairedRows"), bPairedRows);
		double RowSpacing = 300.0;
		Args->TryGetNumberField(TEXT("rowSpacing"), RowSpacing);

		FString HeroMeshPath;
		Args->TryGetStringField(TEXT("heroMesh"), HeroMeshPath);
		bool bHeroAtEnd = true;
		Args->TryGetBoolField(TEXT("heroAtEnd"), bHeroAtEnd);

		bool bSnapDefault = true;
		Args->TryGetBoolField(TEXT("snapToSurface"), bSnapDefault);
		FString Folder;
		Args->TryGetStringField(TEXT("folder"), Folder);
		FString LabelPrefix = TEXT("AxisSlot");
		Args->TryGetStringField(TEXT("labelPrefix"), LabelPrefix);

		const FVector Lateral = FVector::CrossProduct(Direction, FVector::UpVector).GetSafeNormal();
		const float HalfRow = bPairedRows ? static_cast<float>(RowSpacing * 0.5) : 0.0f;

		TArray<TSharedPtr<FJsonValue>> MeshItems;
		auto AddSlot = [&](int32 Index, float LateralOffset)
		{
			const FVector Loc = Origin + Direction * (Spacing * Index) + Lateral * LateralOffset;
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			Item->SetArrayField(TEXT("location"), MakeIntVecArray(Loc));
			Item->SetStringField(TEXT("label"), FString::Printf(TEXT("%s_%d%s"), *LabelPrefix, Index,
				bPairedRows && LateralOffset < 0.0f ? TEXT("_L") : (bPairedRows && LateralOffset > 0.0f ? TEXT("_R") : TEXT(""))));
			Item->SetBoolField(TEXT("snapToSurface"), bSnapDefault);
			MeshItems.Add(MakeShared<FJsonValueObject>(Item));
		};

		for (int32 i = 0; i < SlotCount; ++i)
		{
			if (bPairedRows)
			{
				AddSlot(i, -HalfRow);
				AddSlot(i, HalfRow);
			}
			else
			{
				AddSlot(i, 0.0f);
			}
		}

		UStaticMesh* HeroMesh = nullptr;
		if (!HeroMeshPath.IsEmpty())
		{
			HeroMesh = ResolveStaticMeshPath(HeroMeshPath);
		}
		if (HeroMesh)
		{
			const int32 HeroIndex = bHeroAtEnd ? SlotCount : -1;
			const FVector HeroLoc = Origin + Direction * (Spacing * HeroIndex);
			TSharedRef<FJsonObject> HeroItem = MakeShared<FJsonObject>();
			HeroItem->SetStringField(TEXT("mesh"), HeroMesh->GetPathName());
			HeroItem->SetArrayField(TEXT("location"), MakeIntVecArray(HeroLoc));
			HeroItem->SetStringField(TEXT("label"), FString::Printf(TEXT("%s_hero"), *LabelPrefix));
			HeroItem->SetBoolField(TEXT("snapToSurface"), bSnapDefault);
			MeshItems.Add(MakeShared<FJsonValueObject>(HeroItem));
		}

		TSharedRef<FJsonObject> PlaceArgs = MakeShared<FJsonObject>();
		PlaceArgs->SetArrayField(TEXT("meshes"), MeshItems);
		PlaceArgs->SetBoolField(TEXT("snapToSurface"), bSnapDefault);
		if (!Folder.IsEmpty())
		{
			PlaceArgs->SetStringField(TEXT("folder"), Folder);
		}

		const FString PlaceResult = PlaceMeshes(PlaceArgs);
		TSharedPtr<FJsonObject> PlaceObj;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PlaceResult), PlaceObj) || !PlaceObj.IsValid())
		{
			return PlaceResult;
		}

		bool bSuccess = false;
		PlaceObj->TryGetBoolField(TEXT("success"), bSuccess);
		if (!bSuccess)
		{
			return PlaceResult;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("slotCount"), SlotCount);
		Result->SetNumberField(TEXT("spacing"), Spacing);
		Result->SetBoolField(TEXT("pairedRows"), bPairedRows);
		Result->SetArrayField(TEXT("origin"), MakeIntVecArray(Origin));
		Result->SetArrayField(TEXT("direction"), MakeIntVecArray(Direction));
		Result->SetNumberField(TEXT("totalSlots"), MeshItems.Num());

		TSharedRef<FJsonObject> RoutingObj = MakeShared<FJsonObject>();
		RoutingObj->SetStringField(TEXT("language"), BriefRouting.Language);
		RoutingObj->SetStringField(TEXT("guidance"), BriefRouting.Guidance);
		Result->SetObjectField(TEXT("briefRouting"), RoutingObj);

		double PlacedCount = 0.0;
		PlaceObj->TryGetNumberField(TEXT("placedCount"), PlacedCount);
		Result->SetNumberField(TEXT("placedCount"), PlacedCount);
		const TArray<TSharedPtr<FJsonValue>>* Placed = nullptr;
		if (PlaceObj->TryGetArrayField(TEXT("placed"), Placed))
		{
			Result->SetArrayField(TEXT("placed"), *Placed);
		}
		const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
		if (PlaceObj->TryGetArrayField(TEXT("failed"), Failed))
		{
			Result->SetArrayField(TEXT("failed"), *Failed);
		}
		const TSharedPtr<FJsonObject>* Verdict = nullptr;
		if (PlaceObj->TryGetObjectField(TEXT("geometricVerdict"), Verdict) && Verdict && Verdict->IsValid())
		{
			Result->SetObjectField(TEXT("geometricVerdict"), *Verdict);
		}

		return SuccessJson(Result);
	}

	// ---------------------------------------------------------------------------------------------
	// learn_structure_template — learn a reusable composition rule from a hand-built example.
	//
	// This is learning-FROM-DEMONSTRATION, not weight training: a hand-placed bridge/road/fence is
	// fully observable, so we read every part's mesh + transform and infer the PATTERN (a path, a
	// repeating surface tile, end caps, periodic supports, lateral rails) instead of the literal
	// actors. The same representation describes a road (surface + rail, no support), a bridge
	// (+ periodic piers below), a fence (posts + panels), a wall (segments + caps) — only the parts
	// differ. Roles are inferred from geometry (offset along/across/under the path) and corroborated
	// by authored naming/tags (authored signal wins, same idea as ClassifyRoadZone).
	// ---------------------------------------------------------------------------------------------

	FString GetPrimaryMeshPath(const AActor* Actor)
	{
		TArray<UStaticMeshComponent*> Comps;
		Actor->GetComponents<UStaticMeshComponent>(Comps);
		for (const UStaticMeshComponent* Comp : Comps)
		{
			if (Comp && Comp->GetStaticMesh())
			{
				return Comp->GetStaticMesh()->GetPathName();
			}
		}
		return FString();
	}

	double MedianOf(TArray<double> Values)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		return Values[Values.Num() / 2];
	}

	// Authored-signal role hint from an actor's semantic text (label/name/mesh/folder/tags), EN+CN.
	// Returns "" when nothing matches; the caller prefers this over the geometric guess.
	FString SemanticRoleHint(const FString& Text)
	{
		if (SemanticTextHasAny(Text, { TEXT("pier"), TEXT("pillar"), TEXT("column"), TEXT("support"), TEXT("post"), TEXT("pile"), TEXT("桥墩"), TEXT("立柱"), TEXT("支撑"), TEXT("柱") }))
		{
			return TEXT("support");
		}
		if (SemanticTextHasAny(Text, { TEXT("abutment"), TEXT("headstock"), TEXT("endcap"), TEXT("桥头"), TEXT("桥尾"), TEXT("端头") }))
		{
			return TEXT("endCap");
		}
		if (SemanticTextHasAny(Text, { TEXT("rail"), TEXT("guard"), TEXT("parapet"), TEXT("curb"), TEXT("kerb"), TEXT("balustrade"), TEXT("fence"), TEXT("栏杆"), TEXT("护栏"), TEXT("路缘") }))
		{
			return TEXT("rail");
		}
		if (SemanticTextHasAny(Text, { TEXT("deck"), TEXT("span"), TEXT("surface"), TEXT("slab"), TEXT("road"), TEXT("lane"), TEXT("panel"), TEXT("segment"), TEXT("桥面"), TEXT("路面"), TEXT("面板") }))
		{
			return TEXT("surface");
		}
		return FString();
	}

	TArray<AActor*> ResolveTemplateActors(UWorld* World, const TSharedPtr<FJsonObject>& Args, FString& OutSource)
	{
		TArray<AActor*> Out;

		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (Args->TryGetArrayField(TEXT("actors"), Names) && Names && Names->Num() > 0)
		{
			OutSource = TEXT("actors");
			for (const TSharedPtr<FJsonValue>& Value : *Names)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					if (AActor* Actor = FindActorByNameOrLabel(World, Name))
					{
						Out.AddUnique(Actor);
					}
				}
			}
			return Out;
		}

		FString Folder;
		if (Args->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
		{
			OutSource = TEXT("folder");
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (IsValid(Actor) && Actor->GetFolderPath().ToString().StartsWith(Folder))
				{
					Out.Add(Actor);
				}
			}
			return Out;
		}

		OutSource = TEXT("selection");
		if (GEditor)
		{
			if (USelection* Selection = GEditor->GetSelectedActors())
			{
				TArray<AActor*> Selected;
				Selection->GetSelectedObjects<AActor>(Selected);
				for (AActor* Actor : Selected)
				{
					if (IsValid(Actor))
					{
						Out.Add(Actor);
					}
				}
			}
		}
		return Out;
	}

	// A single placement unit fed to the inference: one mesh at one world transform. A unit is
	// either a whole placed actor OR — when descending a composite — a child actor / mesh component
	// / a single ISM instance.
	struct FStructureUnit { FString Mesh; FVector Center = FVector::ZeroVector; FBox Bounds = FBox(ForceInit); FString Semantic; };

	void AppendActorAsUnit(AActor* Actor, TArray<FStructureUnit>& Out)
	{
		if (!IsValid(Actor))
		{
			return;
		}
		const FBox Bounds = GetActorWorldBounds(Actor);
		if (!Bounds.IsValid)
		{
			return;
		}
		FStructureUnit Unit;
		Unit.Mesh = GetPrimaryMeshPath(Actor);
		Unit.Center = Bounds.GetCenter();
		Unit.Bounds = Bounds;
		Unit.Semantic = BuildActorSemanticText(Actor);
		Out.Add(Unit);
	}

	// Descend a single composite actor (Packed Level Actor / Level Instance / BP instance / attached
	// group) into its REALIZED placement units: each attached child actor, each static-mesh
	// component, and each instance of an ISM — so a packed bridge's repeated piers/deck become
	// individual units. This reads the explicit hierarchy instead of re-deriving it from geometry.
	void AppendCompositeUnits(AActor* Root, TArray<FStructureUnit>& Out)
	{
		TArray<AActor*> Children;
		Root->GetAttachedActors(Children, /*bResetArray=*/true, /*bRecursivelyIncludeAttachedActors=*/true);
		for (AActor* Child : Children)
		{
			AppendActorAsUnit(Child, Out);
		}

		TArray<UStaticMeshComponent*> MeshComps;
		Root->GetComponents<UStaticMeshComponent>(MeshComps);
		for (UStaticMeshComponent* Comp : MeshComps)
		{
			if (!Comp || !Comp->GetStaticMesh())
			{
				continue;
			}
			const FString MeshPath = Comp->GetStaticMesh()->GetPathName();
			FString Semantic = (Comp->GetName() + TEXT(" ") + Comp->GetStaticMesh()->GetName());
			Semantic.ToLowerInline();

			if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(Comp))
			{
				const FBoxSphereBounds MeshBounds = ISM->GetStaticMesh()->GetBounds();
				const int32 InstanceCount = ISM->GetInstanceCount();
				for (int32 i = 0; i < InstanceCount; ++i)
				{
					FTransform InstanceXform;
					if (ISM->GetInstanceTransform(i, InstanceXform, /*bWorldSpace=*/true))
					{
						FStructureUnit Unit;
						Unit.Mesh = MeshPath;
						Unit.Center = InstanceXform.GetLocation();
						Unit.Bounds = MeshBounds.TransformBy(InstanceXform).GetBox();
						Unit.Semantic = Semantic;
						Out.Add(Unit);
					}
				}
			}
			else
			{
				FStructureUnit Unit;
				Unit.Mesh = MeshPath;
				Unit.Center = Comp->Bounds.Origin;
				Unit.Bounds = Comp->Bounds.GetBox();
				Unit.Semantic = Semantic;
				Out.Add(Unit);
			}
		}
	}

	// Resolve placement units. One selected composite actor => read its hierarchy; otherwise each
	// selected actor is itself a unit.
	void GatherStructureUnits(const TArray<AActor*>& Actors, TArray<FStructureUnit>& Out, FString& OutMode)
	{
		if (Actors.Num() == 1)
		{
			AppendCompositeUnits(Actors[0], Out);
			if (Out.Num() >= 2)
			{
				OutMode = TEXT("composite");
				return;
			}
			Out.Reset();
		}
		OutMode = TEXT("actors");
		for (AActor* Actor : Actors)
		{
			AppendActorAsUnit(Actor, Out);
		}
	}

	FString LearnStructureTemplate(const TSharedPtr<FJsonObject>& Args)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return ErrorJson(TEXT("Editor world is not available."));
		}

		FString Source;
		TArray<AActor*> Actors = ResolveTemplateActors(World, Args, Source);
		if (Actors.Num() < 2)
		{
			return ErrorJson(TEXT("Select (or pass 'actors'/'folder') at least 2 actors that make up the example structure."));
		}

		FString TemplateName = TEXT("learned_structure");
		Args->TryGetStringField(TEXT("name"), TemplateName);

		// 1) Perceive each placement UNIT. A single composite actor (Packed Level Actor / Level
		// Instance / BP instance / attached group) is descended into its realized child actors,
		// mesh components and ISM instances — reading the explicit hierarchy when it already exists
		// instead of re-deriving it from many separately-placed actors.
		TArray<FStructureUnit> Units;
		FString GatherMode;
		GatherStructureUnits(Actors, Units, GatherMode);

		struct FInst { FBox Bounds = FBox(ForceInit); FVector Center = FVector::ZeroVector; FString Mesh; FString Semantic; double T = 0.0; double Lateral = 0.0; };
		TArray<FInst> Insts;
		Insts.Reserve(Units.Num());
		for (const FStructureUnit& Unit : Units)
		{
			if (!Unit.Bounds.IsValid)
			{
				continue;
			}
			FInst Inst;
			Inst.Bounds = Unit.Bounds;
			Inst.Center = Unit.Center;
			Inst.Mesh = Unit.Mesh;
			Inst.Semantic = Unit.Semantic;
			Insts.Add(Inst);
		}
		if (Insts.Num() < 2)
		{
			return ErrorJson(TEXT("Could not extract >=2 placement units. Select a multi-part structure, a single composite actor (Packed Level Actor / Level Instance / BP with mesh parts), or pass 'actors'/'folder'."));
		}

		// 2) Infer the main axis: the most-separated pair of centres (XY) sets the direction; every
		// instance is then parameterised by distance ALONG it (t) and perpendicular offset (lateral).
		int32 LoIdx = 0, HiIdx = 1;
		double MaxSep = -1.0;
		for (int32 i = 0; i < Insts.Num(); ++i)
		{
			for (int32 j = i + 1; j < Insts.Num(); ++j)
			{
				const double D = FVector::DistSquaredXY(Insts[i].Center, Insts[j].Center);
				if (D > MaxSep) { MaxSep = D; LoIdx = i; HiIdx = j; }
			}
		}
		const FVector2D Origin2D(Insts[LoIdx].Center.X, Insts[LoIdx].Center.Y);
		FVector2D Dir2D(Insts[HiIdx].Center.X - Origin2D.X, Insts[HiIdx].Center.Y - Origin2D.Y);
		const double AxisLen = Dir2D.Size();
		const bool bHasAxis = AxisLen > 100.0;
		if (bHasAxis)
		{
			Dir2D /= AxisLen;
		}
		const FVector2D Perp2D(-Dir2D.Y, Dir2D.X);
		for (FInst& Inst : Insts)
		{
			const FVector2D P(Inst.Center.X - Origin2D.X, Inst.Center.Y - Origin2D.Y);
			Inst.T = FVector2D::DotProduct(P, Dir2D);
			Inst.Lateral = FVector2D::DotProduct(P, Perp2D);
		}

		// 3) Group by mesh => part types, with per-type stats.
		struct FPartType { FString Mesh; TArray<int32> Idx; double MeanZ = 0.0; double MeanLat = 0.0; double MinT = 0.0; double MaxT = 0.0; double Spacing = 0.0; FString SemHint; };
		TMap<FString, FPartType> TypeMap;
		for (int32 i = 0; i < Insts.Num(); ++i)
		{
			FPartType& PT = TypeMap.FindOrAdd(Insts[i].Mesh);
			PT.Mesh = Insts[i].Mesh;
			PT.Idx.Add(i);
		}
		for (TPair<FString, FPartType>& Pair : TypeMap)
		{
			FPartType& PT = Pair.Value;
			TArray<double> Ts;
			double SumZ = 0.0, SumLat = 0.0;
			TMap<FString, int32> Hints;
			for (int32 i : PT.Idx)
			{
				Ts.Add(Insts[i].T);
				SumZ += Insts[i].Center.Z;
				SumLat += Insts[i].Lateral;
				const FString Hint = SemanticRoleHint(Insts[i].Semantic);
				if (!Hint.IsEmpty())
				{
					Hints.FindOrAdd(Hint)++;
				}
			}
			PT.MeanZ = SumZ / PT.Idx.Num();
			PT.MeanLat = SumLat / PT.Idx.Num();
			Ts.Sort();
			PT.MinT = Ts[0];
			PT.MaxT = Ts.Last();
			if (Ts.Num() >= 2)
			{
				TArray<double> Gaps;
				for (int32 k = 1; k < Ts.Num(); ++k)
				{
					Gaps.Add(Ts[k] - Ts[k - 1]);
				}
				PT.Spacing = MedianOf(Gaps);
			}
			int32 BestHint = -1;
			for (const TPair<FString, int32>& Hint : Hints)
			{
				if (Hint.Value > BestHint) { BestHint = Hint.Value; PT.SemHint = Hint.Key; }
			}
		}

		// 4) The "surface" = most-numerous part type. Its Z is the reference plane; its on-path
		// spacing is the tile length everything else is measured against.
		FString SurfaceMesh;
		int32 SurfaceCount = -1;
		for (const TPair<FString, FPartType>& Pair : TypeMap)
		{
			if (Pair.Value.Idx.Num() > SurfaceCount) { SurfaceCount = Pair.Value.Idx.Num(); SurfaceMesh = Pair.Key; }
		}
		const FPartType& Surface = TypeMap[SurfaceMesh];
		const double SurfaceZ = Surface.MeanZ;
		const double SurfaceLat = Surface.MeanLat;
		const double TileLen = Surface.Spacing > 1.0 ? Surface.Spacing : 0.0;

		// 5) Classify each part type into role + placement mode (authored hint preferred).
		TArray<TSharedPtr<FJsonValue>> Parts;
		TArray<TSharedPtr<FJsonValue>> Measured;
		TArray<FString> SummaryFrags;
		for (const TPair<FString, FPartType>& Pair : TypeMap)
		{
			const FPartType& PT = Pair.Value;
			const double VOff = PT.MeanZ - SurfaceZ;
			const double LatOff = PT.MeanLat - SurfaceLat;
			const int32 Count = PT.Idx.Num();

			FString Role, Mode;
			if (Pair.Key == SurfaceMesh)
			{
				Role = TEXT("surface");
				Mode = TEXT("tile");
			}
			else if (!PT.SemHint.IsEmpty())
			{
				Role = PT.SemHint;
				Mode = (Role == TEXT("surface") || Role == TEXT("rail")) ? TEXT("tile")
					: (Role == TEXT("endCap")) ? TEXT("ends")
					: (Count >= 3 ? TEXT("periodic") : TEXT("ends"));
			}
			else if (VOff < -150.0 && Count >= 2)
			{
				Role = TEXT("support");
				Mode = TEXT("periodic");
			}
			else if (Count <= 2 && bHasAxis && (PT.MinT < AxisLen * 0.15 || PT.MaxT > AxisLen * 0.85))
			{
				Role = TEXT("endCap");
				Mode = TEXT("ends");
			}
			else if (FMath::Abs(LatOff) > FMath::Max(TileLen * 0.3, 100.0))
			{
				Role = TEXT("rail");
				Mode = TEXT("tile");
			}
			else
			{
				Role = TEXT("part");
				Mode = (Count >= 3 ? TEXT("periodic") : TEXT("freeform"));
			}

			TSharedRef<FJsonObject> Part = MakeShared<FJsonObject>();
			Part->SetStringField(TEXT("role"), Role);
			Part->SetStringField(TEXT("mesh"), PT.Mesh);
			Part->SetStringField(TEXT("mode"), Mode);
			Part->SetNumberField(TEXT("count"), Count);
			if (Mode == TEXT("tile") && PT.Spacing > 1.0)
			{
				Part->SetNumberField(TEXT("tileLengthCm"), FMath::RoundToInt(PT.Spacing));
			}
			if (Mode == TEXT("periodic") && PT.Spacing > 1.0)
			{
				Part->SetNumberField(TEXT("periodCm"), FMath::RoundToInt(PT.Spacing));
			}
			if (FMath::Abs(LatOff) > 1.0)
			{
				Part->SetNumberField(TEXT("lateralOffsetCm"), FMath::RoundToInt(LatOff));
			}
			if (FMath::Abs(VOff) > 1.0)
			{
				Part->SetNumberField(TEXT("verticalOffsetCm"), FMath::RoundToInt(VOff));
			}
			if (Role == TEXT("support"))
			{
				Part->SetBoolField(TEXT("dropToGround"), true);
			}
			if (!PT.SemHint.IsEmpty())
			{
				Part->SetStringField(TEXT("authoredHint"), PT.SemHint);
			}
			Parts.Add(MakeShared<FJsonValueObject>(Part));

			// Raw measured stats too, so a wrong role guess is still recoverable downstream.
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("mesh"), PT.Mesh);
			M->SetNumberField(TEXT("count"), Count);
			M->SetNumberField(TEXT("meanZ"), FMath::RoundToInt(PT.MeanZ));
			M->SetNumberField(TEXT("verticalOffsetFromSurface"), FMath::RoundToInt(VOff));
			M->SetNumberField(TEXT("lateralOffsetFromSurface"), FMath::RoundToInt(LatOff));
			M->SetNumberField(TEXT("medianSpacingCm"), FMath::RoundToInt(PT.Spacing));
			Measured.Add(MakeShared<FJsonValueObject>(M));

			SummaryFrags.Add(FString::Printf(TEXT("%s×%d(%s)"), *Role, Count, *Mode));
		}

		// 6) The inferred centreline = ordered surface centres.
		TArray<int32> SurfaceIdx = Surface.Idx;
		SurfaceIdx.Sort([&Insts](int32 A, int32 B) { return Insts[A].T < Insts[B].T; });
		TArray<TSharedPtr<FJsonValue>> PathPts;
		for (int32 i : SurfaceIdx)
		{
			PathPts.Add(MakeShared<FJsonValueArray>(MakeIntVecArray(Insts[i].Center)));
		}

		// 7) Assemble the template.
		TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
		Template->SetStringField(TEXT("schema"), TEXT("WorldDataEngine.StructureTemplate.v1"));
		Template->SetStringField(TEXT("name"), TemplateName);
		Template->SetStringField(TEXT("patternType"), bHasAxis ? TEXT("linear") : TEXT("cluster"));

		TSharedRef<FJsonObject> Axis = MakeShared<FJsonObject>();
		Axis->SetArrayField(TEXT("origin"), MakeIntVecArray(FVector(Origin2D.X, Origin2D.Y, SurfaceZ)));
		TArray<TSharedPtr<FJsonValue>> DirArr;
		DirArr.Add(MakeShared<FJsonValueNumber>(Dir2D.X));
		DirArr.Add(MakeShared<FJsonValueNumber>(Dir2D.Y));
		DirArr.Add(MakeShared<FJsonValueNumber>(0.0));
		Axis->SetArrayField(TEXT("direction"), DirArr);
		Axis->SetNumberField(TEXT("lengthCm"), FMath::RoundToInt(AxisLen));
		Template->SetObjectField(TEXT("axis"), Axis);

		TSharedRef<FJsonObject> Path = MakeShared<FJsonObject>();
		Path->SetStringField(TEXT("kind"), TEXT("polyline"));
		Path->SetArrayField(TEXT("points"), PathPts);
		Template->SetObjectField(TEXT("path"), Path);

		Template->SetArrayField(TEXT("parts"), Parts);
		Template->SetArrayField(TEXT("measured"), Measured);

		TSharedRef<FJsonObject> Src = MakeShared<FJsonObject>();
		Src->SetNumberField(TEXT("unitCount"), Insts.Num());
		Src->SetNumberField(TEXT("partTypeCount"), TypeMap.Num());
		Src->SetStringField(TEXT("from"), Source);
		// "composite" = read from one actor's explicit hierarchy; "actors" = many placed actors.
		Src->SetStringField(TEXT("gatherMode"), GatherMode);
		Template->SetObjectField(TEXT("source"), Src);

		Template->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("%s structure from %d units / %d part types: %s"),
				bHasAxis ? TEXT("Linear") : TEXT("Clustered"), Insts.Num(), TypeMap.Num(), *FString::Join(SummaryFrags, TEXT(", "))));

		// 8) Persist so it can be re-applied later (benign like a screenshot; readOnly to the scene).
		bool bSave = true;
		Args->TryGetBoolField(TEXT("save"), bSave);
		if (bSave)
		{
			const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("structure_templates"));
			IFileManager::Get().MakeDirectory(*Dir, true);
			FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, FString::Printf(TEXT("%s.json"), *FPaths::MakeValidFileName(TemplateName))));
			if (FFileHelper::SaveStringToFile(WorldDataMCP::JsonObjectToString(Template, /*bPretty=*/true), *File))
			{
				Template->SetStringField(TEXT("savedPath"), File);
			}
		}

		return SuccessJson(Template);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"capture_scene_map","description":"Render a top-down (or front/side) ORTHOGRAPHIC map of the whole level and return it as an inline PNG plus a legend that keys dense regions to pixel positions. This is the single best way to grasp scene LAYOUT at a glance — where the road/forest/buildings are — instead of reading a thousand transforms. Frames the level bounds automatically; non-destructive (uses a transient scene capture, does not move the user's viewport).","inputSchema":{"type":"object","properties":{"view":{"type":"string","description":"top (default), front, or side."},"resolution":{"type":"number","description":"Square pixel size. Default 1024, capped 4096."},"paddingPercent":{"type":"number","description":"Extra margin around the level bounds. Default 10."},"drawRegions":{"type":"boolean","description":"Outline dense regions on the image. Default true."},"center":{"type":"object","description":"Optional framing override center [x,y,z] (with extent)."},"extent":{"type":"object","description":"Optional framing override half-size [x,y,z] (with center)."},"inline":{"type":"boolean","description":"Embed the PNG inline so a vision agent can see it. Default true."}}},"annotations":{"title":"Capture Scene Map","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"query_actors_in_region","description":"List actors whose bounds fall inside a spatial region — a box {center,extent} or {min,max}, or a sphere {center,radius}. Returns each actor's category, world center and footprint size, sorted by distance to the region center. Use this to drill into one area instead of dumping the whole level.","inputSchema":{"type":"object","properties":{"center":{"type":"object","description":"Region center [x,y,z]."},"extent":{"type":"object","description":"Box half-size [x,y,z] (with center)."},"radius":{"type":"number","description":"Sphere radius (with center)."},"min":{"type":"object","description":"Box min [x,y,z] (with max)."},"max":{"type":"object","description":"Box max [x,y,z] (with min)."},"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"maxResults":{"type":"number","description":"Default 100, capped 500."}}},"annotations":{"title":"Query Actors In Region","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_nearest_actors","description":"Find the N actors nearest to a point or to another actor, with distances. Origin is 'point' [x,y,z] or 'actor' (name/label, excluded from results).","inputSchema":{"type":"object","properties":{"point":{"type":"object","description":"Origin [x,y,z]."},"actor":{"type":"string","description":"Origin actor name/label (alternative to point)."},"label":{"type":"string","description":"Alias for actor."},"count":{"type":"number","description":"How many to return. Default 10, capped 200."},"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"maxDistance":{"type":"number","description":"Optional max distance in world units."}}},"annotations":{"title":"Find Nearest Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"query_visible_actors_from_camera","description":"Approximate what a camera can see: projects actor bounds into the active perspective viewport or a supplied camera pose, returns screen center/rect, distance/depth, category, and optional occlusion by visibility trace. Use this to connect 3D space to what the user sees on screen.","inputSchema":{"type":"object","properties":{"cameraActor":{"type":"string","description":"Optional camera actor name/label. If omitted, uses the active perspective viewport unless cameraLocation+cameraRotation are supplied."},"cameraLocation":{"type":"object","description":"Optional camera location [x,y,z]. Requires cameraRotation."},"cameraRotation":{"type":"object","description":"Optional camera rotation {pitch,yaw,roll}. Requires cameraLocation."},"fov":{"type":"number","description":"Horizontal field of view in degrees. Defaults to viewport FOV or 90."},"viewportWidth":{"type":"number","description":"Projection width in pixels. Defaults to active viewport or 1920."},"viewportHeight":{"type":"number","description":"Projection height in pixels. Defaults to active viewport or 1080."},"maxDistance":{"type":"number","description":"Maximum actor distance. Default 200000."},"maxResults":{"type":"number","description":"Default 100, capped 500."},"screenMargin":{"type":"number","description":"NDC margin beyond the visible screen to keep near-edge actors. Default 0.15."},"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"categoryFilter":{"type":"string","description":"Optional category substring such as road, building, vegetation, light."},"includeOcclusion":{"type":"boolean","description":"Run center-point visibility traces and report occluders. Default true."}}},"annotations":{"title":"Query Visible Actors From Camera","readOnlyHint":true,"openWorldHint":false}},
)JSON")
TEXT(R"JSON(
{"name":"analyze_spatial_relations","description":"Build a compact spatial relationship graph between actors. Reports relations such as near, overlaps, contains, above/below, north/east/south/west, same elevation, and facing. With actor/label it focuses on one actor; otherwise it summarizes important near/overlap relations among large actors.","inputSchema":{"type":"object","properties":{"actor":{"type":"string","description":"Optional focus actor name/label."},"label":{"type":"string","description":"Alias for actor."},"center":{"type":"object","description":"Optional region center [x,y,z], used with radius."},"radius":{"type":"number","description":"Optional region radius, used with center."},"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"categoryFilter":{"type":"string","description":"Optional category substring such as road, building, vegetation."},"maxActors":{"type":"number","description":"Maximum actors to compare. Default 60, capped 200."},"maxRelations":{"type":"number","description":"Maximum relations returned. Default 200, capped 1000."},"nearDistance":{"type":"number","description":"World-unit edge gap that counts as near. Default 1000."},"includeDirectional":{"type":"boolean","description":"Include directional/facing/elevation labels. Default true."}}},"annotations":{"title":"Analyze Spatial Relations","readOnlyHint":true,"openWorldHint":false}},
{"name":"sample_surface_grid","description":"Sample a top-down grid over a region using line traces. Returns hit point, normal, slope, actor/category, physical material, and height/slope summary. Use before placing PCG/assets to understand terrain and surfaces.","inputSchema":{"type":"object","properties":{"center":{"type":"object","description":"Optional region center [x,y,z]. If omitted, frames the whole level bounds."},"extent":{"type":"object","description":"Optional region half-size [x,y,z]. Must be supplied with center."},"gridX":{"type":"number","description":"Number of samples along X. Default 5, capped 32."},"gridY":{"type":"number","description":"Number of samples along Y. Default 5, capped 32."},"verticalPadding":{"type":"number","description":"Trace padding above and below the region. Default 10000."},"channel":{"type":"string","description":"Collision channel: visibility, camera, worldStatic, worldDynamic, pawn, physicsBody. Default visibility."}}},"annotations":{"title":"Sample Surface Grid","readOnlyHint":true,"openWorldHint":false}},
{"name":"test_placement_clearance","description":"Check whether a proposed box/sphere/capsule placement is clear of blocking overlaps. Returns blocking actors/components and categories without spawning anything. Use before placing assets or characters.","inputSchema":{"type":"object","properties":{"center":{"type":"object","description":"Shape center [x,y,z]."},"shape":{"type":"string","description":"box (default), sphere, or capsule."},"extent":{"type":"object","description":"Box half-size [x,y,z], or fallback sizing for sphere/capsule."},"radius":{"type":"number","description":"Sphere/capsule radius."},"halfHeight":{"type":"number","description":"Capsule half-height."},"channel":{"type":"string","description":"Collision channel: visibility, camera, worldStatic, worldDynamic, pawn, physicsBody. Default visibility."},"ignoreActors":{"type":"array","items":{"type":"string"},"description":"Actor names/labels to ignore."},"maxResults":{"type":"number","description":"Maximum blocking overlaps returned. Default 50, capped 200."}},"required":["center"]},"annotations":{"title":"Test Placement Clearance","readOnlyHint":true,"openWorldHint":false}},
{"name":"analyze_navigation_path","description":"Project start/end to navmesh and find a navigation path. Returns projected endpoints, reachable/partial status, path length, and optional path points. Use this to answer whether a character can travel between two 3D points.","inputSchema":{"type":"object","properties":{"start":{"type":"object","description":"Start location [x,y,z]."},"end":{"type":"object","description":"End location [x,y,z]."},"projectToNav":{"type":"boolean","description":"Project start/end onto navigation first. Default true."},"projectionExtent":{"type":"object","description":"Projection search extent [x,y,z]. Default [500,500,1000]."},"includePathPoints":{"type":"boolean","description":"Return path points. Default true."}},"required":["start","end"]},"annotations":{"title":"Analyze Navigation Path","readOnlyHint":true,"openWorldHint":false}},
{"name":"raycast","description":"Line-trace the editor world and report the first hit: actor, component, impact point, normal and distance. Provide 'start' plus either 'end' or a 'direction'+'length'. Use it to ask 'what is at / along here'.","inputSchema":{"type":"object","properties":{"start":{"type":"object","description":"Ray start [x,y,z]."},"end":{"type":"object","description":"Ray end [x,y,z]."},"direction":{"type":"object","description":"Ray direction [x,y,z] (with length, alternative to end)."},"length":{"type":"number","description":"Ray length when using direction. Default 100000."}},"required":["start"]},"annotations":{"title":"Raycast","readOnlyHint":true,"openWorldHint":false}},
{"name":"what_is_under_camera","description":"Raycast forward from the active perspective viewport camera and report what it is looking at (plus the camera location/rotation). Answers 'what is the user looking at right now'.","inputSchema":{"type":"object","properties":{"length":{"type":"number","description":"Ray length. Default 10000000."}}},"annotations":{"title":"What Is Under Camera","readOnlyHint":true,"openWorldHint":false}},
)JSON")
TEXT(R"JSON(
{"name":"place_meshes","description":"Spawn one or many StaticMesh actors and (by default) SNAP each to the surface directly below its location: traces down, drops the mesh so its bounding-box bottom sits flush on the hit (no float/sink, and the agent does NOT hand-compute the pivot offset), optionally tilts to the surface normal. One call places the whole batch as a single undo. This is the intent-level placement primitive — pair it with raycast/sample_surface_grid/test_placement_clearance to decide WHERE, then place here. By default traces collision geometry (lands on terrain/landscape and collidable meshes) and does NOT hit non-collidable instances such as PCG-generated ISMs — set includeInstances to also rest on the top of instanced-mesh (e.g. PCG ISM) bounds. Returns each placed actor + whether a surface was found and what it rests on.","inputSchema":{"type":"object","properties":{"meshes":{"type":"array","description":"Placement items.","items":{"type":"object","properties":{"mesh":{"type":"string","description":"StaticMesh asset path."},"location":{"type":"object","description":"[x,y,z] or {x,y,z}; XY chooses where, Z is the trace origin (or final Z if not snapping)."},"rotation":{"type":"object","description":"{pitch,yaw,roll}, optional."},"scale":{"type":"number","description":"Uniform scale, or pass [x,y,z]."},"label":{"type":"string"},"snapToSurface":{"type":"boolean"},"alignToNormal":{"type":"boolean"},"includeInstances":{"type":"boolean"}},"required":["mesh"]}},"snapToSurface":{"type":"boolean","description":"Default for all items (default true)."},"alignToNormal":{"type":"boolean","description":"Default for all items (default false)."},"includeInstances":{"type":"boolean","description":"Default for all items: also land on non-collidable instanced meshes (e.g. PCG-generated ISMs) by resting on their bounds top. Default false (collision-only)."},"traceUp":{"type":"number","description":"How far above location.z to start the down-trace. Default 100000."},"traceDown":{"type":"number","description":"How far below to trace. Default 100000."},"folder":{"type":"string","description":"Optional outliner folder for all spawned actors."}},"required":["meshes"]},"annotations":{"title":"Place Meshes","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"place_along_axis","description":"Formal-layout primitive: place meshes at regular intervals along an axis (allée, hedge row, memorial colonnade). Supports paired rows (mirrored across the axis), optional hero terminator mesh at the end, and snap-to-surface. Preferred for language:formal scene briefs instead of random PCG scatter. Returns structured placement summary + briefRouting.","inputSchema":{"type":"object","properties":{"mesh":{"type":"string","description":"StaticMesh asset path for each slot along the axis."},"origin":{"type":"object","description":"Axis start point [x,y,z]. Default 0,0,0."},"direction":{"type":"object","description":"Axis direction [x,y,z]. Default +X. Normalized internally."},"slotCount":{"type":"number","description":"Number of slots along the axis. Default 5, max 500."},"spacing":{"type":"number","description":"Distance between slots in world units (cm). Default 400."},"pairedRows":{"type":"boolean","description":"Mirror a second row on the opposite side of the axis (allée). Default false."},"rowSpacing":{"type":"number","description":"Lateral distance between paired rows. Default 300."},"heroMesh":{"type":"string","description":"Optional terminator mesh at the axis end (or start when heroAtEnd=false)."},"heroAtEnd":{"type":"boolean","description":"Place hero after the last slot (default true); false = before slot 0."},"snapToSurface":{"type":"boolean","description":"Snap each slot to ground. Default true."},"folder":{"type":"string","description":"Outliner folder for spawned actors."},"labelPrefix":{"type":"string","description":"Actor label prefix. Default AxisSlot."}},"required":["mesh"]},"annotations":{"title":"Place Along Axis","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
)JSON")
TEXT(R"JSON(
{"name":"learn_structure_template","description":"Learn a reusable, parameter-driven STRUCTURE TEMPLATE from a hand-built example. Source is, in order: the selected actors, an 'actors' list, or a 'folder'. When the source is ONE composite actor (Packed Level Actor / Level Instance / Blueprint instance / attached group), its explicit hierarchy is read directly — child actors, mesh components and every ISM instance become units — instead of re-deriving the layout from geometry. Infers the composition rule (a path/axis, a repeating surface tile, end caps, periodic supports like bridge piers, lateral rails) by grouping units by mesh and reading each part's offset along/across/under the path, corroborated by authored naming/tags. Generalises to any linear-modular structure: bridge, road, fence, wall, pipeline (only the parts differ). Read-only to the scene; returns the template JSON plus raw 'measured' stats and a 'summary', and optionally saves it under Saved/UEBridgeMCP/structure_templates. Learning-from-demonstration, no model training; pair with a compose/apply step to rebuild along a new path.","inputSchema":{"type":"object","properties":{"actors":{"type":"array","items":{"type":"string"},"description":"Actor names/labels that make up the example. One composite actor => its hierarchy is read. If omitted, uses the current editor selection."},"folder":{"type":"string","description":"Alternatively, learn from all actors under this outliner folder (prefix match)."},"name":{"type":"string","description":"Template name, used for the saved file. Default 'learned_structure'."},"save":{"type":"boolean","description":"Save the template JSON to disk. Default true."}}},"annotations":{"title":"Learn Structure Template","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"list_placements","description":"UNIFIED placed-object read-back across ALL four scatter routes at once — StaticMesh actors (place_meshes/hand), PCG-spawned ISM, foliage HISM, and plain ISM (lay_meshes_along_spline). One call instead of querying actors + enumerate_pcg_instances + foliage separately. Each unit reports its 'source' (actor|pcg|foliage|ism) so you know WHICH route produced it, plus category, mesh, world center/size. Returns total + bySource + byCategory histograms (always complete even when the list is capped) and a capped 'placements' list. Optional region/source/category/mesh filters. This is the single source of truth for what is placed in the scene.","inputSchema":{"type":"object","properties":{"center":{"type":"object","description":"Region center [x,y,z] (with extent or radius)."},"extent":{"type":"object","description":"Box half-size [x,y,z] (with center)."},"radius":{"type":"number","description":"Sphere radius (with center)."},"min":{"type":"object","description":"Box min [x,y,z] (with max)."},"max":{"type":"object","description":"Box max [x,y,z] (with min)."},"source":{"type":"string","description":"Filter by route: actor, pcg, foliage, or ism."},"category":{"type":"string","description":"Filter by category e.g. vegetation, road, building."},"mesh":{"type":"string","description":"Case-sensitive mesh-name substring filter."},"maxResults":{"type":"number","description":"Cap on returned units. Default 200, capped 2000. Histograms stay complete."},"includeList":{"type":"boolean","description":"Return the per-unit list, not just histograms. Default true."}}},"annotations":{"title":"List Placements","readOnlyHint":true,"openWorldHint":false}},
{"name":"verify_scene","description":"Run the DETERMINISTIC geometric critic over the whole scene (or a region) and RECORD the authoritative verdict the save gate trusts. Checks every placed StaticMesh actor for floating / buried / over-void / mutual-overlap (and out-of-bounds when a region is given) — all fact-based, no LLM. This is the server-side, skill-independent half of the verify loop: generation marks the scene 'needs verification' and ONLY this tool clears it; save_current_level / save_all_dirty refuse while a scene is unverified or its last verdict was FAIL. Does NOT judge aesthetics (those need vision) — on a geometric pass it returns the brief concept + a directive to capture views and score the design 0-30. Read-only to the scene.","inputSchema":{"type":"object","properties":{"center":{"type":"object","description":"Optional review-region center [x,y,z] (with extent). Enables the out-of-bounds check and scopes the review."},"extent":{"type":"object","description":"Optional review-region half-size [x,y,z] (with center)."},"min":{"type":"object","description":"Optional region min [x,y,z] (with max)."},"max":{"type":"object","description":"Optional region max [x,y,z] (with min)."},"floatThreshold":{"type":"number","description":"cm above the surface before an object counts as floating. Default 50."},"buryThreshold":{"type":"number","description":"cm below the surface before an object counts as buried. Default 50."},"overlapTolerance":{"type":"number","description":"cm of interpenetration before a pair counts as overlapping. Default 25."}}},"annotations":{"title":"Verify Scene","readOnlyHint":true,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("capture_scene_map")) { OutResult = CaptureSceneMap(Args); return true; }
	if (ToolName == TEXT("query_actors_in_region")) { OutResult = QueryActorsInRegion(Args); return true; }
	if (ToolName == TEXT("find_nearest_actors")) { OutResult = FindNearestActors(Args); return true; }
	if (ToolName == TEXT("query_visible_actors_from_camera")) { OutResult = QueryVisibleActorsFromCamera(Args); return true; }
	if (ToolName == TEXT("analyze_spatial_relations")) { OutResult = AnalyzeSpatialRelations(Args); return true; }
	if (ToolName == TEXT("sample_surface_grid")) { OutResult = SampleSurfaceGrid(Args); return true; }
	if (ToolName == TEXT("test_placement_clearance")) { OutResult = TestPlacementClearance(Args); return true; }
	if (ToolName == TEXT("analyze_navigation_path")) { OutResult = AnalyzeNavigationPath(Args); return true; }
	if (ToolName == TEXT("raycast")) { OutResult = Raycast(Args); return true; }
	if (ToolName == TEXT("what_is_under_camera")) { OutResult = WhatIsUnderCamera(Args); return true; }
	if (ToolName == TEXT("place_meshes")) { OutResult = PlaceMeshes(Args); return true; }
	if (ToolName == TEXT("place_along_axis")) { OutResult = PlaceAlongAxis(Args); return true; }
	if (ToolName == TEXT("list_placements")) { OutResult = ListPlacements(Args); return true; }
	if (ToolName == TEXT("learn_structure_template")) { OutResult = LearnStructureTemplate(Args); return true; }
	if (ToolName == TEXT("verify_scene")) { OutResult = VerifyScene(Args); return true; }
	return false;
}
}
}
