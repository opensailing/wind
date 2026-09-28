#include "StudioPlanarSurface.h"
#include "StudioSegmentCoverage.h"
#include "Algo/Sort.h"
#include <limits>

namespace StudioPlanarPrivate
{
bool Cancelled(const FStudioLoadCancellation& C) { return C && C->load(std::memory_order_relaxed); }
double Cross(const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; }
bool Finite(const FVector2D& P) { return FMath::IsFinite(P.X) && FMath::IsFinite(P.Y); }
bool Contains(const FBox2D& B, const FVector2D& P)
{ return P.X >= B.Min.X && P.X <= B.Max.X && P.Y >= B.Min.Y && P.Y <= B.Max.Y; }
}

FStudioPlanarSurface::FStudioPlanarSurface(TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry)
    : Points(MoveTemp(Geometry)) {}

FVector2D FStudioPlanarSurface::Point(int32 Row) const
{ const auto& P = Points->Positions[Row]; return FVector2D(P.X, P.Y); }

FStudioPlanarSurfaceResult FStudioPlanarSurface::Create(
    TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry, TArray<FIntVector> Triangles,
    int64 MaxIndexBytes, const FStudioLoadCancellation& Cancellation)
{
    using namespace StudioPlanarPrivate;
    auto Fail = [](const TCHAR* Error) { return FStudioPlanarSurfaceResult{ {}, Error }; };
    if (Cancelled(Cancellation)) return Fail(TEXT("Surface indexing cancelled."));
    if (Geometry->Positions.Num() < 3 || Geometry->Positions.Num() > 1000000 ||
        Triangles.IsEmpty() || Triangles.Num() > 2000000)
        return Fail(TEXT("Surface point or triangle count is outside supported limits."));
    // Conservative allocation allowance includes copied triangle capacity, order,
    // and twice as many nodes as faces. Shared source geometry is counted by its reader.
    const int64 Allowed = int64(Triangles.GetAllocatedSize()) +
        int64(Triangles.Num()) * (sizeof(int32) + 2 * sizeof(FNode));
    if (MaxIndexBytes <= 0 || Allowed > MaxIndexBytes)
        return Fail(TEXT("Surface index exceeds its memory budget."));
    for (int32 I = 0; I < Geometry->Positions.Num(); ++I)
    {
        if ((I & 1023) == 0 && Cancelled(Cancellation)) return Fail(TEXT("Surface indexing cancelled."));
        const auto& P = Geometry->Positions[I];
        if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z) || P.Z != 0)
            return Fail(TEXT("Surface interpolation requires finite source XY points with zero Z."));
    }
    for (int32 I = 0; I < Triangles.Num(); ++I)
    {
        if ((I & 1023) == 0 && Cancelled(Cancellation)) return Fail(TEXT("Surface indexing cancelled."));
        const auto& T = Triangles[I];
        if (T.X < 0 || T.Y < 0 || T.Z < 0 || T.X >= Geometry->Positions.Num() ||
            T.Y >= Geometry->Positions.Num() || T.Z >= Geometry->Positions.Num())
            return Fail(TEXT("Surface triangle references an unavailable source row."));
        const auto& A = Geometry->Positions[T.X]; const auto& B = Geometry->Positions[T.Y]; const auto& C = Geometry->Positions[T.Z];
        const double Area2 = Cross(FVector2D(B.X - A.X, B.Y - A.Y), FVector2D(C.X - A.X, C.Y - A.Y));
        if (!FMath::IsFinite(Area2) || Area2 <= 0)
            return Fail(TEXT("Surface triangle is degenerate, inverted or outside numeric range."));
    }
    // Private construction: publish only after a complete, uncancelled index.
    TSharedRef<FStudioPlanarSurface, ESPMode::ThreadSafe> Surface =
        MakeShareable(new FStudioPlanarSurface(MoveTemp(Geometry)));
    Surface->Faces = MoveTemp(Triangles);
    Surface->Order.SetNumUninitialized(Surface->Faces.Num());
    for (int32 I = 0; I < Surface->Order.Num(); ++I) Surface->Order[I] = I;
    Surface->Nodes.Reserve(Surface->Faces.Num());
    int32 Root = INDEX_NONE;
    if (!Surface->BuildNode(0, Surface->Order.Num(), Root, Cancellation) || Cancelled(Cancellation))
        return Fail(TEXT("Surface indexing cancelled."));
    if (Surface->IndexBytes() > MaxIndexBytes) return Fail(TEXT("Surface index exceeds its memory budget."));
    return { Surface, {} };
}

bool FStudioPlanarSurface::BuildNode(int32 First, int32 Count, int32& OutNode,
    const FStudioLoadCancellation& Cancellation)
{
    if (StudioPlanarPrivate::Cancelled(Cancellation)) return false;
    FNode Node; Node.First = First; Node.Count = Count;
    for (int32 I = First; I < First + Count; ++I)
    {
        if ((I & 1023) == 0 && StudioPlanarPrivate::Cancelled(Cancellation)) return false;
        const auto& T = Faces[Order[I]];
        Node.Bounds += Point(T.X); Node.Bounds += Point(T.Y); Node.Bounds += Point(T.Z);
    }
    OutNode = Nodes.Add(Node);
    if (Count <= 8) return true;
    const bool bX = Node.Bounds.GetSize().X >= Node.Bounds.GetSize().Y;
    Algo::Sort(TArrayView<int32>(Order.GetData() + First, Count), [&](int32 A, int32 B)
    {
        auto Center = [&](int32 I)
        {
            const auto& T = Faces[I];
            // Divide before adding to avoid overflow of otherwise finite coordinates.
            return bX ? Point(T.X).X / 3 + Point(T.Y).X / 3 + Point(T.Z).X / 3 :
                Point(T.X).Y / 3 + Point(T.Y).Y / 3 + Point(T.Z).Y / 3;
        };
        const double X = Center(A), Y = Center(B);
        return X == Y ? A < B : X < Y;
    });
    const int32 LeftCount = Count / 2; int32 Left, Right;
    if (!BuildNode(First, LeftCount, Left, Cancellation) ||
        !BuildNode(First + LeftCount, Count - LeftCount, Right, Cancellation)) return false;
    Nodes[OutNode].Left = Left; Nodes[OutNode].Right = Right; Nodes[OutNode].Count = 0;
    return true;
}

bool FStudioPlanarSurface::TriangleLocation(int32 Face, const FVector2D& P, FStudioSurfaceLocation& Out) const
{
    const auto& T = Faces[Face]; const auto A = Point(T.X), B = Point(T.Y), C = Point(T.Z);
    if (P == A) { Out = { Face, T, FVector(1, 0, 0) }; return true; }
    if (P == B) { Out = { Face, T, FVector(0, 1, 0) }; return true; }
    if (P == C) { Out = { Face, T, FVector(0, 0, 1) }; return true; }
    const auto AB = B - A, AC = C - A, AP = P - A;
    const double Denominator = StudioPlanarPrivate::Cross(AB, AC);
    const double Y = StudioPlanarPrivate::Cross(AP, AC) / Denominator;
    const double Z = StudioPlanarPrivate::Cross(AB, AP) / Denominator;
    FVector W(1 - Y - Z, Y, Z);
    if (!FMath::IsFinite(W.X) || !FMath::IsFinite(W.Y) || !FMath::IsFinite(W.Z) || W.GetMin() < -1.e-12 || W.GetMax() > 1 + 1.e-12)
        return false;
    W.X = FMath::Clamp(W.X, 0., 1.); W.Y = FMath::Clamp(W.Y, 0., 1.); W.Z = FMath::Clamp(W.Z, 0., 1.);
    W /= W.X + W.Y + W.Z;
    Out = { Face, T, W }; return true;
}

bool FStudioPlanarSurface::Locate(const FVector2D& P, FStudioSurfaceLocation& Out) const
{
    Out = {};
    if (!StudioPlanarPrivate::Finite(P) || Nodes.IsEmpty()) return false;
    // Balanced median splits and the two-million-face input cap imply depth < 32.
    // A fixed traversal stack avoids one heap allocation per streamline sample.
    int32 Stack[64]; int32 Count = 1; Stack[0] = 0;
    while (Count)
    {
        const FNode& N = Nodes[Stack[--Count]];
        if (!StudioPlanarPrivate::Contains(N.Bounds, P)) continue;
        if (N.Count)
        {
            for (int32 I = N.First; I < N.First + N.Count; ++I)
            {
                const int32 Face = Order[I];
                if (Out.Triangle != INDEX_NONE && Face > Out.Triangle) continue;
                FStudioSurfaceLocation Candidate;
                if (TriangleLocation(Face, P, Candidate)) Out = Candidate;
            }
        }
        else { Stack[Count++] = N.Right; Stack[Count++] = N.Left; }
    }
    return Out.Triangle != INDEX_NONE;
}

bool FStudioPlanarSurface::Sample(const FVector2D& P, const TArray<double>& Values, double& Out) const
{
    Out = std::numeric_limits<double>::quiet_NaN();
    if (Values.Num() != Points->Positions.Num()) return false;
    FStudioSurfaceLocation L; if (!Locate(P, L)) return false;
    const double A = Values[L.Rows.X], B = Values[L.Rows.Y], C = Values[L.Rows.Z];
    if (!FMath::IsFinite(A) || !FMath::IsFinite(B) || !FMath::IsFinite(C)) return false;
    const double Value = A * L.Weights.X + B * L.Weights.Y + C * L.Weights.Z;
    if (!FMath::IsFinite(Value)) return false;
    Out = Value; return true;
}

int64 FStudioPlanarSurface::IndexBytes() const
{ return Faces.GetAllocatedSize() + Order.GetAllocatedSize() + Nodes.GetAllocatedSize(); }

bool FStudioPlanarSurface::SupportsSegment(const FVector2D& A,const FVector2D& B,
    const FStudioLoadCancellation& Cancellation) const
{
    using namespace StudioPlanarPrivate;
    if(!Finite(A)||!Finite(B)||Nodes.IsEmpty()||Cancelled(Cancellation))return false;
    FBox2D Segment(ForceInit);Segment+=A;Segment+=B;
    int32 Stack[64],Count=1,Visited=0;Stack[0]=0;
    TArray<FVector2D> Intervals;
    while(Count)
    {
        if(Cancelled(Cancellation))return false;
        const auto& N=Nodes[Stack[--Count]];
        if(N.Bounds.Max.X<Segment.Min.X||N.Bounds.Min.X>Segment.Max.X||
            N.Bounds.Max.Y<Segment.Min.Y||N.Bounds.Min.Y>Segment.Max.Y)continue;
        if(!N.Count){Stack[Count++]=N.Right;Stack[Count++]=N.Left;continue;}
        for(int32 I=N.First;I<N.First+N.Count;++I)
        {
            if(++Visited>65536)return false;
            const auto& T=Faces[Order[I]];FVector2D Interval;
            if(StudioSegmentCoverage::Triangle(A,B,Point(T.X),Point(T.Y),Point(T.Z),Interval))Intervals.Add(Interval);
        }
    }
    return !Cancelled(Cancellation)&&StudioSegmentCoverage::Complete(Intervals);
}
