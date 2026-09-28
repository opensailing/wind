#include "StudioInspectionObjects.h"
#include "String/LexFromString.h"
#include <limits>

namespace
{
bool FinitePoint(const FVector& P)
{
    return FMath::IsFinite(P.X)&&FMath::IsFinite(P.Y)&&FMath::IsFinite(P.Z)&&P.GetAbsMax()<=1.e8;
}
bool UnitNormal(const FVector& N)
{
    return FinitePoint(N)&&FMath::Abs(N.SizeSquared()-1.)<=1.e-10;
}
bool InspectionText(const FString& S,int32 Limit)
{
    if(S.IsEmpty()||S.Len()>Limit||S!=S.TrimStartAndEnd())return false;
    for(TCHAR C:S)if(FChar::IsControl(C))return false;
    return true;
}
bool Hash(const FString& S)
{
    if(S.Len()!=64)return false;
    for(TCHAR C:S)if(!((C>='0'&&C<='9')||(C>='a'&&C<='f')))return false;
    return true;
}
bool FieldId(const FString& S)
{
    if(S.Len()>128)return false;
    for(TCHAR C:S)if(!FChar::IsAlnum(C)&&C!='_'&&C!='-'&&C!='.')return false;
    return true;
}
bool SliceGeometry(const FStudioSliceObject& S)
{
    return FinitePoint(S.Origin)&&UnitNormal(S.Normal)&&FMath::IsFinite(S.Opacity)&&S.Opacity>=0&&S.Opacity<=1;
}
bool ProbeGeometry(const FStudioProbeObject& P)
{
    return (P.Kind==EStudioProbeKind::Point||P.Kind==EStudioProbeKind::Line)&&
        (P.Method==EStudioProbeMethod::Interpolated||P.Method==EStudioProbeMethod::OriginalPoint)&&
        FinitePoint(P.A)&&FinitePoint(P.B)&&P.Samples>=2&&P.Samples<=StudioInspectionObjects::MaxLineSamples&&FieldId(P.Field)&&
        (P.Method==EStudioProbeMethod::OriginalPoint?(P.Kind==EStudioProbeKind::Point&&P.PointId.IsSet()):!P.PointId.IsSet());
}
double MetersPerUnit(const FString& Unit)
{
    if(Unit==TEXT("m"))return 1.;
    if(Unit==TEXT("cm"))return .01;
    if(Unit==TEXT("mm"))return .001;
    if(Unit==TEXT("in"))return .0254;
    if(Unit==TEXT("ft"))return .3048;
    return 0;
}
bool RulerGeometry(const FStudioRulerObject& R)
{
    return (R.Kind==EStudioRulerKind::Distance||R.Kind==EStudioRulerKind::Angle)&&
        FinitePoint(R.A)&&FinitePoint(R.B)&&FinitePoint(R.C)&&MetersPerUnit(R.Unit)>0;
}
bool SeedGeometry(const FStudioSeedObject& S)
{
    if(uint8(S.Kind)>uint8(EStudioSeedKind::Points)||S.Count<1||S.Count>StudioInspectionObjects::MaxSeedsPerObject||
        S.InletAxis<0||S.InletAxis>2||!FinitePoint(S.A)||!FinitePoint(S.B)||!FinitePoint(S.C)||
        S.Points.Num()>StudioInspectionObjects::MaxSeedsPerObject)return false;
    if(S.Kind==EStudioSeedKind::Line&&S.A==S.B)return false;
    if(S.Kind==EStudioSeedKind::Plane)
    {
        const double Area=FVector::CrossProduct(S.B,S.C).Size(),Scale=S.B.Size()*S.C.Size();
        if(Scale<=0||!FMath::IsFinite(Area)||Area<=Scale*1.e-10)return false;
    }
    if(S.Kind==EStudioSeedKind::Points&&S.Points.IsEmpty())return false;
    TSet<FVector> Seen;
    for(const auto& P:S.Points){if(!FinitePoint(P)||Seen.Contains(P))return false;Seen.Add(P);}
    return true;
}
bool ValidBounds(const FBox& B)
{
    return B.IsValid&&FinitePoint(B.Min)&&FinitePoint(B.Max)&&
        B.Min.X<B.Max.X&&B.Min.Y<B.Max.Y&&B.Min.Z<B.Max.Z;
}
TArray<TSharedPtr<FJsonValue>> PointJSON(const FVector& P)
{
    return {MakeShared<FJsonValueNumber>(P.X),MakeShared<FJsonValueNumber>(P.Y),MakeShared<FJsonValueNumber>(P.Z)};
}
bool ReadPoint(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,FVector& P)
{
    const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if(!O->TryGetArrayField(Key,A)||A->Num()!=3)return false;
    for(int32 I=0;I<3;++I)if(!(*A)[I]||!(*A)[I]->TryGetNumber(P[I]))return false;
    return FinitePoint(P);
}
TSharedRef<FJsonObject> ObjectJSON(const FStudioInspectionObject& Object)
{
    auto O=MakeShared<FJsonObject>(),Source=MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("dataset"),Object.Source.Dataset);
    Source->SetStringField(TEXT("metadataSHA256"),Object.Source.MetadataSHA256);
    Source->SetStringField(TEXT("payloadSHA256"),Object.Source.PayloadSHA256);
    O->SetObjectField(TEXT("source"),Source);
    O->SetStringField(TEXT("id"),Object.Id.ToString());O->SetStringField(TEXT("name"),Object.Name);
    O->SetBoolField(TEXT("visible"),Object.bVisible);
    return O;
}
bool ReadObject(const TSharedPtr<FJsonObject>& O,FStudioInspectionObject& Object)
{
    const TSharedPtr<FJsonObject>* Source=nullptr;FString Id;
    return O&&O->TryGetStringField(TEXT("id"),Id)&&FGuid::Parse(Id,Object.Id)&&
        O->TryGetStringField(TEXT("name"),Object.Name)&&O->TryGetBoolField(TEXT("visible"),Object.bVisible)&&
        O->TryGetObjectField(TEXT("source"),Source)&&(*Source)->TryGetStringField(TEXT("dataset"),Object.Source.Dataset)&&
        (*Source)->TryGetStringField(TEXT("metadataSHA256"),Object.Source.MetadataSHA256)&&
        (*Source)->TryGetStringField(TEXT("payloadSHA256"),Object.Source.PayloadSHA256);
}
bool ReadInspectionInteger(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32& Out,int32 Minimum,int32 Maximum)
{
    double V;
    if(!O->TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<Minimum||V>Maximum||FMath::FloorToDouble(V)!=V)return false;
    Out=int32(V);return true;
}
}

bool FStudioInspectionSource::operator==(const FStudioInspectionSource& Other) const
{
    return Dataset==Other.Dataset&&MetadataSHA256==Other.MetadataSHA256&&PayloadSHA256==Other.PayloadSHA256;
}
bool FStudioInspectionObject::Equals(const FStudioInspectionObject& Other) const
{
    return Id==Other.Id&&Name==Other.Name&&Source==Other.Source&&bVisible==Other.bVisible;
}
bool FStudioSliceObject::operator==(const FStudioSliceObject& Other) const
{ return Equals(Other)&&Origin==Other.Origin&&Normal==Other.Normal&&Opacity==Other.Opacity; }
bool FStudioProbeObject::operator==(const FStudioProbeObject& Other) const
{
    return Equals(Other)&&Kind==Other.Kind&&Method==Other.Method&&A==Other.A&&B==Other.B&&
        Samples==Other.Samples&&Field==Other.Field&&PointId==Other.PointId;
}
bool FStudioRulerObject::operator==(const FStudioRulerObject& Other) const
{ return Equals(Other)&&Kind==Other.Kind&&A==Other.A&&B==Other.B&&C==Other.C&&Unit==Other.Unit; }
bool FStudioSeedObject::operator==(const FStudioSeedObject& Other) const
{ return Equals(Other)&&Kind==Other.Kind&&Count==Other.Count&&InletAxis==Other.InletAxis&&
    bUpperFace==Other.bUpperFace&&A==Other.A&&B==Other.B&&C==Other.C&&Points==Other.Points; }
bool FStudioInspectionObjects::operator==(const FStudioInspectionObjects& Other) const
{ return Slices==Other.Slices&&Probes==Other.Probes&&Rulers==Other.Rulers&&Seeds==Other.Seeds; }

bool StudioInspectionObjects::IsValid(const FStudioInspectionSource& Source)
{
    return InspectionText(Source.Dataset,256)&&Hash(Source.MetadataSHA256)&&
        (Source.PayloadSHA256.IsEmpty()||Hash(Source.PayloadSHA256));
}
bool StudioInspectionObjects::IsValid(const FStudioInspectionObjects& Objects,FString& Error)
{
    Error=TEXT("Invalid inspection objects. The previous objects have been kept.");
    if(Objects.Slices.Num()>MaxObjectsPerKind||Objects.Probes.Num()>MaxObjectsPerKind||Objects.Rulers.Num()>MaxObjectsPerKind||Objects.Seeds.Num()>MaxObjectsPerKind)
    {Error=TEXT("Each inspection collection is limited to 128 objects.");return false;}
    TSet<FGuid> Ids;TSet<FString> Names;
    auto Common=[&](const FStudioInspectionObject& Object)
    {
        if(!Object.Id.IsValid()||!InspectionText(Object.Name,120)||!IsValid(Object.Source)||
            Ids.Contains(Object.Id)||Names.Contains(Object.Name.ToLower()))return false;
        Ids.Add(Object.Id);Names.Add(Object.Name.ToLower());return true;
    };
    for(const auto& S:Objects.Slices)if(!Common(S)||!SliceGeometry(S))return false;
    for(const auto& P:Objects.Probes)if(!Common(P)||!ProbeGeometry(P))return false;
    for(const auto& R:Objects.Rulers)if(!Common(R)||!RulerGeometry(R))return false;
    int32 SeedCount=0;
    for(const auto& S:Objects.Seeds)
    {
        if(!Common(S)||!SeedGeometry(S))return false;
        SeedCount+=S.Kind==EStudioSeedKind::Points?S.Points.Num():S.Count;
        if(SeedCount>MaxTotalSeeds){Error=TEXT("Streamline seed sets are limited to 4,096 total positions.");return false;}
    }
    Error.Empty();return true;
}
TSharedRef<FJsonObject> StudioInspectionObjects::ToJSON(const FStudioInspectionObjects& Objects)
{
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("version"),CurrentVersion);
    TArray<TSharedPtr<FJsonValue>> Slices,Probes,Rulers,Seeds;
    for(const auto& S:Objects.Slices)
    {
        auto Item=ObjectJSON(S);Item->SetArrayField(TEXT("origin"),PointJSON(S.Origin));
        Item->SetArrayField(TEXT("normal"),PointJSON(S.Normal));Item->SetNumberField(TEXT("opacity"),S.Opacity);
        Slices.Add(MakeShared<FJsonValueObject>(Item));
    }
    for(const auto& P:Objects.Probes)
    {
        auto Item=ObjectJSON(P);Item->SetStringField(TEXT("kind"),P.Kind==EStudioProbeKind::Point?TEXT("point"):
            P.Kind==EStudioProbeKind::Line?TEXT("line"):TEXT("invalid"));
        Item->SetStringField(TEXT("method"),P.Method==EStudioProbeMethod::Interpolated?TEXT("interpolated"):
            P.Method==EStudioProbeMethod::OriginalPoint?TEXT("originalPoint"):TEXT("invalid"));
        Item->SetArrayField(TEXT("a"),PointJSON(P.A));Item->SetArrayField(TEXT("b"),PointJSON(P.B));
        Item->SetNumberField(TEXT("samples"),P.Samples);Item->SetStringField(TEXT("field"),P.Field);
        if(P.PointId.IsSet())Item->SetStringField(TEXT("pointId"),FString::Printf(TEXT("%lld"),P.PointId.GetValue()));
        else Item->SetField(TEXT("pointId"),MakeShared<FJsonValueNull>());
        Probes.Add(MakeShared<FJsonValueObject>(Item));
    }
    for(const auto& R:Objects.Rulers)
    {
        auto Item=ObjectJSON(R);Item->SetStringField(TEXT("kind"),R.Kind==EStudioRulerKind::Distance?TEXT("distance"):
            R.Kind==EStudioRulerKind::Angle?TEXT("angle"):TEXT("invalid"));
        Item->SetArrayField(TEXT("a"),PointJSON(R.A));Item->SetArrayField(TEXT("b"),PointJSON(R.B));
        Item->SetArrayField(TEXT("c"),PointJSON(R.C));Item->SetStringField(TEXT("unit"),R.Unit);
        Rulers.Add(MakeShared<FJsonValueObject>(Item));
    }
    for(const auto& S:Objects.Seeds)
    {
        auto Item=ObjectJSON(S);Item->SetNumberField(TEXT("kind"),uint8(S.Kind));Item->SetNumberField(TEXT("count"),S.Count);
        Item->SetNumberField(TEXT("inletAxis"),S.InletAxis);Item->SetBoolField(TEXT("upperFace"),S.bUpperFace);
        Item->SetArrayField(TEXT("a"),PointJSON(S.A));Item->SetArrayField(TEXT("b"),PointJSON(S.B));Item->SetArrayField(TEXT("c"),PointJSON(S.C));
        TArray<TSharedPtr<FJsonValue>> Positions;
        for(const auto& P:S.Points)Positions.Add(MakeShared<FJsonValueArray>(PointJSON(P)));
        Item->SetArrayField(TEXT("points"),Positions);Seeds.Add(MakeShared<FJsonValueObject>(Item));
    }
    O->SetArrayField(TEXT("slices"),Slices);O->SetArrayField(TEXT("probes"),Probes);O->SetArrayField(TEXT("rulers"),Rulers);O->SetArrayField(TEXT("seeds"),Seeds);
    return O;
}
bool StudioInspectionObjects::FromJSON(const TSharedPtr<FJsonObject>& O,FStudioInspectionObjects& Out,FString& Error)
{
    Error=TEXT("Invalid inspection objects. The previous objects have been kept.");
    int32 Version;const TArray<TSharedPtr<FJsonValue>> *Slices=nullptr,*Probes=nullptr,*Rulers=nullptr,*Seeds=nullptr;
    if(!O||!ReadInspectionInteger(O,TEXT("version"),Version,1,CurrentVersion)||!O->TryGetArrayField(TEXT("slices"),Slices)||
        !O->TryGetArrayField(TEXT("probes"),Probes)||!O->TryGetArrayField(TEXT("rulers"),Rulers)||
        Slices->Num()>MaxObjectsPerKind||Probes->Num()>MaxObjectsPerKind||Rulers->Num()>MaxObjectsPerKind)return false;
    FStudioInspectionObjects Candidate;
    for(const auto& Value:*Slices)
    {
        const TSharedPtr<FJsonObject>* Item=nullptr;FStudioSliceObject S;
        if(!Value||!Value->TryGetObject(Item)||!ReadObject(*Item,S)||!ReadPoint(*Item,TEXT("origin"),S.Origin)||
            !ReadPoint(*Item,TEXT("normal"),S.Normal)||!(*Item)->TryGetNumberField(TEXT("opacity"),S.Opacity))return false;
        Candidate.Slices.Add(MoveTemp(S));
    }
    for(const auto& Value:*Probes)
    {
        const TSharedPtr<FJsonObject>* Item=nullptr;FStudioProbeObject P;FString Kind,Method;
        if(!Value||!Value->TryGetObject(Item)||!ReadObject(*Item,P)||!(*Item)->TryGetStringField(TEXT("kind"),Kind)||
            !(*Item)->TryGetStringField(TEXT("method"),Method)||!ReadPoint(*Item,TEXT("a"),P.A)||!ReadPoint(*Item,TEXT("b"),P.B)||
            !ReadInspectionInteger(*Item,TEXT("samples"),P.Samples,2,MaxLineSamples)||!(*Item)->TryGetStringField(TEXT("field"),P.Field))return false;
        if(Kind==TEXT("point"))P.Kind=EStudioProbeKind::Point;
        else if(Kind==TEXT("line"))P.Kind=EStudioProbeKind::Line;else return false;
        if(Method==TEXT("interpolated"))P.Method=EStudioProbeMethod::Interpolated;
        else if(Method==TEXT("originalPoint"))P.Method=EStudioProbeMethod::OriginalPoint;else return false;
        const auto PointId=(*Item)->TryGetField(TEXT("pointId"));
        if(!PointId)return false;
        if(PointId->Type!=EJson::Null)
        {
            FString Id;int64 Parsed;
            if(!PointId->TryGetString(Id)||Id.Len()>20||!LexTryParseString(Parsed,*Id)||
                FString::Printf(TEXT("%lld"),Parsed)!=Id)return false;
            P.PointId=Parsed;
        }
        Candidate.Probes.Add(MoveTemp(P));
    }
    for(const auto& Value:*Rulers)
    {
        const TSharedPtr<FJsonObject>* Item=nullptr;FStudioRulerObject R;FString Kind;
        if(!Value||!Value->TryGetObject(Item)||!ReadObject(*Item,R)||!(*Item)->TryGetStringField(TEXT("kind"),Kind)||
            !ReadPoint(*Item,TEXT("a"),R.A)||!ReadPoint(*Item,TEXT("b"),R.B)||!ReadPoint(*Item,TEXT("c"),R.C)||
            !(*Item)->TryGetStringField(TEXT("unit"),R.Unit))return false;
        if(Kind==TEXT("distance"))R.Kind=EStudioRulerKind::Distance;
        else if(Kind==TEXT("angle"))R.Kind=EStudioRulerKind::Angle;else return false;
        Candidate.Rulers.Add(MoveTemp(R));
    }
    if(Version>=2)
    {
        if(!O->TryGetArrayField(TEXT("seeds"),Seeds)||Seeds->Num()>MaxObjectsPerKind)return false;
        for(const auto& Value:*Seeds)
        {
            const TSharedPtr<FJsonObject>* Item=nullptr;FStudioSeedObject S;int32 Kind;
            const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;
            if(!Value||!Value->TryGetObject(Item)||!ReadObject(*Item,S)||!ReadInspectionInteger(*Item,TEXT("kind"),Kind,0,3)||
                !ReadInspectionInteger(*Item,TEXT("count"),S.Count,1,MaxSeedsPerObject)||!ReadInspectionInteger(*Item,TEXT("inletAxis"),S.InletAxis,0,2)||
                !(*Item)->TryGetBoolField(TEXT("upperFace"),S.bUpperFace)||!ReadPoint(*Item,TEXT("a"),S.A)||!ReadPoint(*Item,TEXT("b"),S.B)||
                !ReadPoint(*Item,TEXT("c"),S.C)||!(*Item)->TryGetArrayField(TEXT("points"),Points)||Points->Num()>MaxSeedsPerObject)return false;
            S.Kind=EStudioSeedKind(Kind);
            for(const auto& Point:*Points)
            {
                const TArray<TSharedPtr<FJsonValue>>* XYZ=nullptr;FVector P;
                if(!Point||!Point->TryGetArray(XYZ)||XYZ->Num()!=3)return false;
                for(int32 I=0;I<3;++I)if(!(*XYZ)[I]||!(*XYZ)[I]->TryGetNumber(P[I]))return false;
                S.Points.Add(P);
            }
            Candidate.Seeds.Add(MoveTemp(S));
        }
    }
    if(!IsValid(Candidate,Error))return false;
    Out=MoveTemp(Candidate);return true;
}

bool StudioInspectionObjects::SliceRange(const FVector& N,const FBox& B,double& Minimum,double& Maximum)
{
    if(!UnitNormal(N)||!ValidBounds(B))return false;
    const double Center=FVector::DotProduct(N,B.GetCenter());
    const double Radius=FVector::DotProduct(N.GetAbs(),B.GetExtent());
    Minimum=Center-Radius;Maximum=Center+Radius;return true;
}
bool StudioInspectionObjects::MoveSlice(FStudioSliceObject& S,double Position)
{
    if(!SliceGeometry(S)||!FMath::IsFinite(Position))return false;
    const FVector Origin=S.Origin+S.Normal*(Position-FVector::DotProduct(S.Normal,S.Origin))/S.Normal.SizeSquared();
    if(!FinitePoint(Origin))return false;
    S.Origin=Origin;return true;
}
TArray<FVector> StudioInspectionObjects::SlicePolygon(const FStudioSliceObject& S,const FBox& B)
{
    TArray<FVector> Points;
    if(!SliceGeometry(S)||!ValidBounds(B))return Points;
    FVector Corners[8];double Distances[8];
    const double Scale=FMath::Max3(1.,B.Min.GetAbsMax(),B.Max.GetAbsMax());
    const double Epsilon=FMath::Max(B.GetSize().GetMax()*1.e-12,Scale*std::numeric_limits<double>::epsilon()*16.);
    for(int32 I=0;I<8;++I)
    {
        Corners[I]=FVector(I&1?B.Max.X:B.Min.X,I&2?B.Max.Y:B.Min.Y,I&4?B.Max.Z:B.Min.Z);
        Distances[I]=FVector::DotProduct(Corners[I]-S.Origin,S.Normal);
    }
    auto Add=[&](FVector P)
    {
        for(const auto& Existing:Points)if((Existing-P).SizeSquared()<=Epsilon*Epsilon)return;
        Points.Add(P);
    };
    for(int32 I=0;I<8;++I)for(int32 Axis=0;Axis<3;++Axis)
    {
        if(I&(1<<Axis))continue;
        const int32 J=I|(1<<Axis);const double A=Distances[I],C=Distances[J];
        if(FMath::Abs(A)<=Epsilon)Add(Corners[I]);
        if(FMath::Abs(C)<=Epsilon)Add(Corners[J]);
        if((A< -Epsilon&&C>Epsilon)||(C< -Epsilon&&A>Epsilon))Add(FMath::Lerp(Corners[I],Corners[J],A/(A-C)));
    }
    if(Points.Num()<3)return {};
    FVector Center=FVector::ZeroVector;for(const auto& P:Points)Center+=P;Center/=Points.Num();
    const FVector Abs=S.Normal.GetAbs();
    const FVector Axis=Abs.X<=Abs.Y&&Abs.X<=Abs.Z?FVector::ForwardVector:Abs.Y<=Abs.Z?FVector::RightVector:FVector::UpVector;
    const FVector U=FVector::CrossProduct(Axis,S.Normal).GetSafeNormal(),V=FVector::CrossProduct(S.Normal,U);
    Points.Sort([&](const FVector& A,const FVector& C)
    {
        const auto PA=A-Center,PC=C-Center;
        return FMath::Atan2(FVector::DotProduct(PA,V),FVector::DotProduct(PA,U))<
            FMath::Atan2(FVector::DotProduct(PC,V),FVector::DotProduct(PC,U));
    });
    double TwiceArea=0;
    for(int32 I=0;I<Points.Num();++I)TwiceArea+=FVector::DotProduct(
        FVector::CrossProduct(Points[I]-Center,Points[(I+1)%Points.Num()]-Center),S.Normal);
    return TwiceArea>Epsilon*Epsilon?Points:TArray<FVector>();
}
bool StudioInspectionObjects::IntersectSlice(const FStudioSliceObject& S,const FVector& O,const FVector& D,FVector& Out)
{
    if(!SliceGeometry(S)||!FinitePoint(O)||!FinitePoint(D))return false;
    const double Length=D.Size(),Denominator=FVector::DotProduct(D,S.Normal);
    if(Length==0||FMath::Abs(Denominator)<=Length*1.e-12)return false;
    const double T=FVector::DotProduct(S.Origin-O,S.Normal)/Denominator;
    if(!FMath::IsFinite(T)||T<0)return false;
    const FVector Hit=O+D*T;if(!FinitePoint(Hit))return false;
    Out=Hit;return true;
}
TArray<FVector> StudioInspectionObjects::ProbeLocations(const FStudioProbeObject& P)
{
    if(!ProbeGeometry(P)||P.Method==EStudioProbeMethod::OriginalPoint)return {};
    if(P.Kind==EStudioProbeKind::Point)return {P.A};
    TArray<FVector> Locations;Locations.Reserve(P.Samples);
    for(int32 I=0;I<P.Samples;++I)Locations.Add(I==0?P.A:I==P.Samples-1?P.B:FMath::Lerp(P.A,P.B,double(I)/(P.Samples-1)));
    return Locations;
}
TOptional<double> StudioInspectionObjects::Measurement(const FStudioRulerObject& R)
{
    if(!RulerGeometry(R))return {};
    if(R.Kind==EStudioRulerKind::Distance)return (R.B-R.A).Size()/MetersPerUnit(R.Unit);
    const FVector A=R.A-R.B,C=R.C-R.B;
    const double LA=A.Size(),LC=C.Size();if(LA==0||LC==0)return {};
    // atan2 retains useful precision for nearly parallel and antiparallel arms.
    const FVector U=A/LA,V=C/LC;
    return FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(U,V).Size(),FVector::DotProduct(U,V)));
}
