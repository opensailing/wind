#pragma once
#include "CoreMinimal.h"

/** Session layout only; fractions are relative to the available travel area. */
struct FStudioFloatingPaneState
{
    FVector2D Position=FVector2D::ZeroVector;
    bool bMoved=false;
    bool bMinimized=false;
};
namespace StudioFloatingPanes
{
    inline const TArray<FName>& Names()
    {
        static const TArray<FName> Value={TEXT("Status"),TEXT("Tools"),TEXT("Axes"),TEXT("Legend"),TEXT("View")};return Value;
    }
    inline FVector2D Travel(FVector2D Viewport,FVector2D Pane)
    {return FVector2D(FMath::Max(0.,Viewport.X-Pane.X-16),FMath::Max(0.,Viewport.Y-Pane.Y-16));}
    inline FVector2D Position(const FStudioFloatingPaneState& State,FVector2D Viewport,FVector2D Pane,FVector2D Anchor,FVector2D Offset)
    {
        const auto Room=Travel(Viewport,Pane);
        const auto P=State.bMoved?State.Position*Room:Anchor*Room+Offset;
        return FVector2D(8+FMath::Clamp(P.X,0.,Room.X),8+FMath::Clamp(P.Y,0.,Room.Y));
    }
}
