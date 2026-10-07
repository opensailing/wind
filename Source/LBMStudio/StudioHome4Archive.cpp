#include "StudioHome4Archive.h"
#include "StudioHome4ArchivePrivate.h"
#include "StudioFileDialog.h"
#include "StudioPointRecording.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include <cmath>

struct FStudioHome4ArchiveWork
{
    std::atomic<EStudioHome4ArchiveState> State{EStudioHome4ArchiveState::Working};
    std::atomic<int32> Completed{0};std::atomic<int64> Bytes{0};int32 Total=0;
    FStudioLoadCancellation Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
};
namespace StudioHome4ArchivePrivate
{
constexpr int64 WorkingBytes=256LL*1024*1024;
bool ReserveManifestBytes(const FString& SnapshotJSON,int64& UsedBytes,int64 MaximumBytes)
{
    const int64 RowBytes=int64(FTCHARToUTF8(*SnapshotJSON).Length())+1; // Array comma/framing allowance.
    if(UsedBytes<0||MaximumBytes<UsedBytes||RowBytes>MaximumBytes-UsedBytes)return false;
    UsedBytes+=RowBytes;return true;
}
TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> V)
{TArray<TSharedPtr<FJsonValue>> A;for(double N:V)A.Add(MakeShared<FJsonValueNumber>(N));return A;}
TArray<TSharedPtr<FJsonValue>> Triple(const FVector& V){return Numbers({V.X,V.Y,V.Z});}
TArray<TSharedPtr<FJsonValue>> Triple(const FIntVector& V){return Numbers({double(V.X),double(V.Y),double(V.Z)});}
void Optional(FJsonObject& O,const TCHAR* Key,const TOptional<double>& V)
{if(V)O.SetNumberField(Key,V.GetValue());else O.SetField(Key,MakeShared<FJsonValueNull>());}
bool MetadataNumber(const FJsonObject& O,const TCHAR* Key,TOptional<double>& Value,FString& Error)
{
    if(!O.HasField(Key)||O.HasTypedField<EJson::Null>(Key))return true;
    double V;if(!O.TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<=0){Error=FString(TEXT("Invalid original unit/reference anchor: "))+Key;return false;}
    if(Value&&Value.GetValue()!=V){Error=FString(TEXT("Explicit import value conflicts with original metadata: "))+Key;return false;}
    Value=V;return true;
}
bool Object(const FJsonObject& O,const TCHAR* Key,const FJsonObject*& Out,FString& Error)
{
    Out=nullptr;if(!O.HasField(Key))return true;const TSharedPtr<FJsonObject>* P=nullptr;
    if(!O.TryGetObjectField(Key,P)||!P||!P->IsValid()){Error=FString(TEXT("Original metadata requires an object: "))+Key;return false;}Out=P->Get();return true;
}
bool ContractString(const FJsonObject& O,const TCHAR* Key,FString& Value,FString& Error)
{
    if(!O.HasField(Key))return true;FString V;
    if(!O.TryGetStringField(Key,V)||!CleanText(V,128)){Error=FString(TEXT("Invalid original convention: "))+Key;return false;}
    if(!Value.IsEmpty()&&Value!=V){Error=FString(TEXT("Explicit import convention conflicts with original metadata: "))+Key;return false;}Value=V;return true;
}
struct FSnapshot
{
    FIntVector Original,Minimum,Maximum,Selected;
    FVector Origin,Spacing;
    int32 Iteration=0;double Time=0;
    TMap<FString,TArray<double>> Core,Fields;
    TMap<FString,FString> Units,Expressions;
    TSet<FString> Derived;
    TSharedPtr<FJsonObject> RunSpec;
    FString RunIdentity;
};
int32 Node(const FIntVector& P,const FIntVector& N){return P.X+N.X*(P.Y+N.Y*P.Z);}
int64 Count(const FIntVector& N){return int64(N.X)*N.Y*N.Z;}
bool IsNumeric(const FStudioHome4ArchiveMember& M){return M.DType.Len()>1&&M.DType[1]!='S'&&M.DType[1]!='U';}
TArray<double> Canonical(const FNumericArray& A,const FString& Order,const FIntVector& N)
{
    TArray<double> Values;Values.SetNumUninitialized(int32(Count(N)));
    const int32 AX=Order.Find(TEXT("x")),AY=Order.Find(TEXT("y")),AZ=Order.Find(TEXT("z"));
    for(int32 Z=0;Z<N.Z;++Z)for(int32 Y=0;Y<N.Y;++Y)for(int32 X=0;X<N.X;++X)
    {
        int64 I[3];I[AX]=X;I[AY]=Y;I[AZ]=Z;
        const int64 Raw=A.Member.bFortran?I[0]+A.Member.Shape[0]*(I[1]+A.Member.Shape[1]*I[2]):
            I[2]+A.Member.Shape[2]*(I[1]+A.Member.Shape[1]*I[0]);
        Values[X+N.X*(Y+N.Y*Z)]=A.Values[int32(Raw)];
    }
    return Values;
}
TArray<double> Select(const TArray<double>& Full,const FSnapshot& S,int32 Stride)
{
    TArray<double> A;A.Reserve(int32(Count(S.Selected)));
    for(int32 Z=S.Minimum.Z;Z<S.Maximum.Z;Z+=Stride)for(int32 Y=S.Minimum.Y;Y<S.Maximum.Y;Y+=Stride)for(int32 X=S.Minimum.X;X<S.Maximum.X;X+=Stride)
    {
        A.Add(Full[Node(FIntVector(X,Y,Z),S.Original)]);
    }
    return A;
}
bool TripleArray(const FNumericArray& A,FVector& V,bool Positive)
{
    if(A.Member.DType[1]=='b'||!(A.Member.Shape.IsEmpty()||A.Member.Shape==TArray<int64>{3}))return false;
    for(int32 K=0;K<3;++K){V[K]=A.Values[A.Member.Shape.IsEmpty()?0:K];if(!FMath::IsFinite(V[K])||(Positive&&V[K]<=0))return false;}return true;
}
bool DerivedField(const FString& K){return K==TEXT("q")||K==TEXT("helicity")||K==TEXT("divergence")||K.StartsWith(TEXT("vorticity"));}
bool AirMasked(const FString& K)
{return DerivedField(K)||K.StartsWith(TEXT("a3_"))||K.StartsWith(TEXT("a4_"))||K.StartsWith(TEXT("S"))||K.StartsWith(TEXT("F_"))||K==TEXT("pressure")||K==TEXT("p_star")||K==TEXT("Pi_h")||K==TEXT("Pi_h0")||K==TEXT("dissipation");}
bool SourceUnitContract(const FString& K,const FStudioHome4ArchiveMapping& M,const FJsonObject* Declared,FString& Error)
{
    if(!Declared||!Declared->HasField(K))return true;
    FString Expected;
    if(K==TEXT("ux")||K==TEXT("uy")||K==TEXT("uz"))Expected=M.VelocityUnits==TEXT("physical")?TEXT("m/s"):TEXT("lu_velocity");
    else if(K==TEXT("phi")||K==TEXT("solid")||K==TEXT("p_star"))Expected=TEXT("1");
    else if(K==TEXT("rho"))Expected=TEXT("lu_density");
    else if(K==TEXT("nu"))Expected=TEXT("cells2/step");
    else if(K==TEXT("Pi_h")||K==TEXT("Pi_h0"))Expected=TEXT("lu_pressure");
    else if(TArray<FString>{TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")}.Contains(K))Expected=TEXT("1/step");
    if(Expected.IsEmpty())return true;
    FString Supplied;
    if(!Declared->TryGetStringField(K,Supplied)||Supplied!=Expected)
    {Error=TEXT("Original archive.fieldUnits conflicts with the fixed source contract for ")+K+TEXT("; expected ")+Expected+TEXT(". Values were not converted.");return false;}
    return true;
}
bool Unit(const FString& K,const FStudioHome4ArchiveMapping& M,bool Physical,const FJsonObject* Declared,FString& Label,double& Scale,FString& Error)
{
    Scale=1;const double Length=Physical&&M.CoordinateUnits==TEXT("lattice")?M.DxMeters.GetValue():1.;
    const double Velocity=Physical&&M.VelocityUnits==TEXT("lattice")?M.DxMeters.GetValue()/M.DtSeconds.GetValue():1.;
    const FString C=Physical||M.CoordinateUnits==TEXT("physical")?TEXT("m"):TEXT("lu_length");
    const FString V=Physical||M.VelocityUnits==TEXT("physical")?TEXT("m/s"):TEXT("lu_velocity");
    const bool SI=C==TEXT("m")&&V==TEXT("m/s");
    if(K==TEXT("ux")||K==TEXT("uy")||K==TEXT("uz")||K==TEXT("speed")){Label=V;Scale=Velocity;}
    else if(K==TEXT("phi")||K==TEXT("solid")||K.EndsWith(TEXT("_support"))||K==TEXT("derivative_valid")){Label=TEXT("1");}
    else if(K.StartsWith(TEXT("vorticity"))||K==TEXT("divergence")){Label=SI?TEXT("1/s"):TEXT("(")+V+TEXT(")/(")+C+TEXT(")");Scale=Velocity/Length;}
    else if(K==TEXT("q")){Label=SI?TEXT("1/s2"):TEXT("((")+V+TEXT(")/(")+C+TEXT("))^2");Scale=FMath::Square(Velocity/Length);}
    else if(K==TEXT("helicity")){Label=SI?TEXT("m/s2"):TEXT("(")+V+TEXT(")^2/(")+C+TEXT(")");Scale=Velocity*Velocity/Length;}
    else if(K==TEXT("p_star")){Label=TEXT("1");}
    else if(K==TEXT("rho")){Label=Physical&&M.DensityReferenceKgM3?TEXT("kg/m3"):TEXT("lu_density");if(Physical&&M.DensityReferenceKgM3)Scale=M.DensityReferenceKgM3.GetValue();}
    else if(K==TEXT("Pi_h")||K==TEXT("Pi_h0")||K==TEXT("pressure"))
    {
        const bool Available=M.DxMeters&&M.DtSeconds&&M.DensityReferenceKgM3;
        if(K==TEXT("pressure")&&Physical&&!Available){Error=TEXT("Physical WB pressure requires original dx, dt and density reference.");return false;}
        Label=Physical&&Available?TEXT("Pa"):TEXT("lu_pressure");if(Physical&&Available)Scale=M.DensityReferenceKgM3.GetValue()*FMath::Square(M.DxMeters.GetValue()/M.DtSeconds.GetValue());
    }
    else if(K==TEXT("nu")||K==TEXT("dissipation"))
    {const bool Available=Physical&&M.DxMeters&&M.DtSeconds;const bool Diss=K==TEXT("dissipation");Label=Available?(Diss?TEXT("m2/s3"):TEXT("m2/s")):(Diss?TEXT("cells2/step3"):TEXT("cells2/step"));if(Available)Scale=FMath::Square(M.DxMeters.GetValue())/std::pow(M.DtSeconds.GetValue(),Diss?3:1);}
    else if(TArray<FString>{TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")}.Contains(K))
    {Label=Physical&&M.DtSeconds?TEXT("1/s"):TEXT("1/step");if(Physical&&M.DtSeconds)Scale=1/M.DtSeconds.GetValue();}
    else if(!Declared||!Declared->TryGetStringField(K,Label)||!CleanText(Label,128))
    {Error=TEXT("Original field requires explicit archive.fieldUnits; normalization is unknown: ")+K;return false;}
    if(!FMath::IsFinite(Scale)){Error=TEXT("Original field unit conversion overflows: ")+K;return false;}return true;
}
bool Snapshot(FNpzReader& Reader,const FStudioHome4ArchiveRequest& R,FSnapshot& S,FString& E,const FStudioLoadCancellation& Cancel)
{
    FNumericArray A;const auto& Members=Reader.Members();const auto* Phi=Members.FindByPredicate([](const auto& M){return M.Name==TEXT("phi");});
    if(!Phi||Phi->Shape.Num()!=3||Phi->Count<1||Phi->Count>StudioHome4Archives::MaximumSourceNodes){E=TEXT("Original phi requires a 3-D grid of at most two million nodes.");return false;}
    for(int32 K=0;K<3;++K){S.Original[K]=int32(Phi->Shape[R.Mapping.AxisOrder.Find(FString::Chr(TCHAR('x'+K)))]);if(S.Original[K]<2){E=TEXT("Original grid must have at least two nodes on every axis.");return false;}}
    S.Minimum=R.CropMinimum.Get(FIntVector::ZeroValue);S.Maximum=R.CropMaximum.Get(S.Original);
    for(int32 K=0;K<3;++K)
    {if(S.Minimum[K]<0||S.Minimum[K]>=S.Maximum[K]||S.Maximum[K]>S.Original[K]){E=TEXT("Crop must use valid original XYZ half-open bounds.");return false;}S.Selected[K]=(S.Maximum[K]-S.Minimum[K]+R.PreviewStride-1)/R.PreviewStride;if(S.Selected[K]<2||S.Selected[K]>512){E=TEXT("Selected grid requires 2–512 nodes per axis. Adjust crop/stride.");return false;}}
    if(Count(S.Selected)>StudioHome4Archives::MaximumSelectedNodes){E=TEXT("Selected grid exceeds one million nodes. Adjust crop/stride.");return false;}
    if(!Reader.Read(TEXT("iteration"),A,E)||!A.Member.Shape.IsEmpty()||(A.Member.DType[1]!='i'&&A.Member.DType[1]!='u')||A.Values[0]<0||A.Values[0]>MAX_int32)
    {E=TEXT("iteration must be an original nonnegative integer int32 solver step.");return false;}S.Iteration=int32(A.Values[0]);
    S.Time=R.Mapping.DtSeconds?R.Mapping.TimeOriginSeconds+S.Iteration*R.Mapping.DtSeconds.GetValue():double(S.Iteration);
    if(!FMath::IsFinite(S.Time)){E=TEXT("Original physical timestamp overflows.");return false;}
    if(!Reader.Read(TEXT("origin"),A,E)||!TripleArray(A,S.Origin,false)||!Reader.Read(TEXT("spacing"),A,E)||!TripleArray(A,S.Spacing,true))
    {E=TEXT("Original origin/spacing must be finite numeric scalars or triples; spacing must be positive.");return false;}
    if(R.Mapping.MetadataOrder==TEXT("array")){const FVector Origin=S.Origin,Spacing=S.Spacing;for(int32 K=0;K<3;++K){const int32 Axis=R.Mapping.AxisOrder.Find(FString::Chr(TCHAR('x'+K)));S.Origin[K]=Origin[Axis];S.Spacing[K]=Spacing[Axis];}}
    if(Members.ContainsByPredicate([](const auto& M){return M.Name==TEXT("run_spec");}))
    {if(!Reader.Read(TEXT("run_spec"),A,E,false)||!JSON(A.StringValue,S.RunSpec,E))return false;}
    const FJsonObject* Archive=nullptr,*Declared=nullptr;
    if(S.RunSpec)
    {
        auto M=R.Mapping;if(!StudioHome4Archives::MappingFromMetadata(*S.RunSpec,M,E))return false;
        if(M.AxisOrder!=R.Mapping.AxisOrder||M.MetadataOrder!=R.Mapping.MetadataOrder||M.CoordinateUnits!=R.Mapping.CoordinateUnits||M.VelocityUnits!=R.Mapping.VelocityUnits||M.DxMeters!=R.Mapping.DxMeters||M.DtSeconds!=R.Mapping.DtSeconds||M.DensityReferenceKgM3!=R.Mapping.DensityReferenceKgM3||M.LiquidMinimum!=R.Mapping.LiquidMinimum||M.TimeOriginSeconds!=R.Mapping.TimeOriginSeconds)
        {E=TEXT("Import mapping must include the original metadata anchors and conventions. Inspect and confirm them first.");return false;}
        if(!Object(*S.RunSpec,TEXT("archive"),Archive,E))return false;
        auto Identity=MakeShared<FJsonObject>();for(const TCHAR* K:{TEXT("runId"),TEXT("recipeId"),TEXT("lineageId"),TEXT("units"),TEXT("reference"),TEXT("fluids"),TEXT("archive")})
        {
            if(S.RunSpec->HasField(K))Identity->SetField(K,S.RunSpec->Values.FindChecked(K));
        }
        S.RunIdentity=Serialize(Identity);
    }
    if(Archive&&!Object(*Archive,TEXT("fieldUnits"),Declared,E))return false;
    const TArray<FString> Core={TEXT("ux"),TEXT("uy"),TEXT("uz"),TEXT("phi"),TEXT("solid")};
    TArray<FString> IDs=Core;
    for(const auto& M:Members)
    {
        if(IDs.Contains(M.Name)||M.Name==TEXT("iteration")||M.Name==TEXT("origin")||M.Name==TEXT("spacing")||M.Name==TEXT("run_spec"))continue;
        const bool Known=TArray<FString>{TEXT("p_star"),TEXT("Pi_h"),TEXT("Pi_h0"),TEXT("rho"),TEXT("nu"),TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")}.Contains(M.Name);
        if((Known||(Declared&&Declared->HasField(M.Name)))&&(M.Shape!=Phi->Shape||!IsNumeric(M)))
        {E=TEXT("Original optional/declared diagnostic field has incompatible shape or dtype: ")+M.Name;return false;}
        if((M.Name.StartsWith(TEXT("a3_"))||M.Name.StartsWith(TEXT("a4_"))||M.Name.StartsWith(TEXT("grad_phi_"))||M.Name.StartsWith(TEXT("F_"))||M.Name==TEXT("tau_fld")||M.Name==TEXT("sdf"))&&(!Declared||!Declared->HasField(M.Name)))
        {E=TEXT("Original advanced field needs explicit archive.fieldUnits; normalization is unknown: ")+M.Name;return false;}
        if(M.Shape!=Phi->Shape||!IsNumeric(M))continue; // Listed with original header in source manifest.
        if(M.Name==TEXT("speed")||M.Name.EndsWith(TEXT("_support"))||M.Name==TEXT("derivative_valid")||DerivedField(M.Name)||M.Name==TEXT("pressure")||M.Name==TEXT("dissipation"))
        {E=TEXT("Source array collides with a reserved derived field; it cannot be replaced: ")+M.Name;return false;}
        IDs.Add(M.Name);
    }
    const int32 DerivedCount=3+(R.bDerivatives?8:0)+(!R.PressureConvention.IsEmpty()?1:0)+(IDs.Contains(TEXT("nu"))&&IDs.Contains(TEXT("Sxx"))&&IDs.Contains(TEXT("Syy"))&&IDs.Contains(TEXT("Szz"))&&IDs.Contains(TEXT("Sxy"))&&IDs.Contains(TEXT("Sxz"))&&IDs.Contains(TEXT("Syz"))?1:0);
    if(IDs.Num()+DerivedCount>32||Count(S.Original)*40+Count(S.Selected)*(IDs.Num()+DerivedCount)*8+48LL*1024*1024>WorkingBytes)
    {E=TEXT("Selected field/grid combination exceeds the 32-field or 256 MiB import budget. Reduce the selected grid.");return false;}
    for(const FString& K:IDs)
    {
        if(Cancelled(Cancel))return false;
        if(!SourceUnitContract(K,R.Mapping,Declared,E))return false;
        if(!Reader.Read(K,A,E)||A.Member.Shape!=Phi->Shape){E=TEXT("Source field is missing or has a different original grid: ")+K;return false;}
        auto Full=Canonical(A,R.Mapping.AxisOrder,S.Original);A.Values.Empty();
        if(K==TEXT("solid"))for(double V:Full)if(V!=0&&V!=1){E=TEXT("Original solid must be explicitly binary.");return false;}
        S.Fields.Add(K,Select(Full,S,R.PreviewStride));if(Core.Contains(K))S.Core.Add(K,MoveTemp(Full));
    }
    const int32 SelectedCount=int32(Count(S.Selected));auto Add=[&](const FString& K,const FString& Expression)->TArray<double>&
    {S.Derived.Add(K);S.Expressions.Add(K,Expression);auto& V=S.Fields.Add(K);V.SetNumZeroed(SelectedCount);return V;};
    auto& Speed=Add(TEXT("speed"),TEXT("sqrt(ux^2+uy^2+uz^2) from original velocity components"));
    for(int32 I=0;I<SelectedCount;++I)Speed[I]=std::hypot(S.Fields[TEXT("ux")][I],S.Fields[TEXT("uy")][I],S.Fields[TEXT("uz")][I]);
    Add(TEXT("solid_support"),TEXT("1 only if complete original preview-node support is nonsolid"));
    Add(TEXT("liquid_support"),TEXT("1 only if complete original preview-node support is liquid and nonsolid"));
    // Summed original invalid masks bound work independently of preview stride.
    for(const FString K:{TEXT("solid_support"),TEXT("liquid_support")})
    {
        const FIntVector PN=S.Original+FIntVector(1,1,1);TArray<int32> Prefix;Prefix.SetNumZeroed(int32(Count(PN)));
        for(int32 Z=0;Z<S.Original.Z;++Z)for(int32 Y=0;Y<S.Original.Y;++Y)for(int32 X=0;X<S.Original.X;++X)
        {
            const int32 I=Node(FIntVector(X,Y,Z),S.Original);const bool Bad=S.Core[TEXT("solid")][I]!=0||(K==TEXT("liquid_support")&&S.Core[TEXT("phi")][I]<R.Mapping.LiquidMinimum);
            const FIntVector P(X+1,Y+1,Z+1);Prefix[Node(P,PN)]=int32(Bad)+Prefix[Node(P-FIntVector(1,0,0),PN)]+Prefix[Node(P-FIntVector(0,1,0),PN)]+Prefix[Node(P-FIntVector(0,0,1),PN)]-Prefix[Node(P-FIntVector(1,1,0),PN)]-Prefix[Node(P-FIntVector(1,0,1),PN)]-Prefix[Node(P-FIntVector(0,1,1),PN)]+Prefix[Node(P-FIntVector(1,1,1),PN)];
            if((I&4095)==0&&Cancelled(Cancel))return false;
        }
        int32 Row=0;for(int32 Z=S.Minimum.Z;Z<S.Maximum.Z;Z+=R.PreviewStride)for(int32 Y=S.Minimum.Y;Y<S.Maximum.Y;Y+=R.PreviewStride)for(int32 X=S.Minimum.X;X<S.Maximum.X;X+=R.PreviewStride)
        {
            const FIntVector P(X,Y,Z);const int32 Radius=R.PreviewStride==1?0:R.PreviewStride;FIntVector Lo,Hi;
            for(int32 J=0;J<3;++J){Lo[J]=FMath::Max(0,P[J]-Radius);Hi[J]=FMath::Min(S.Original[J],P[J]+Radius+1);}
            int32 Invalid=0;for(int32 C=0;C<8;++C){FIntVector At;int32 Highs=0;for(int32 J=0;J<3;++J){bool High=(C&(1<<J))!=0;At[J]=High?Hi[J]:Lo[J];Highs+=High;}Invalid+=(Highs%2?1:-1)*Prefix[Node(At,PN)];}
            S.Fields[K][Row++]=Invalid==0?1.:0.;
        }
    }
    if(R.bDerivatives)
    {
        const TArray<FString> Keys={TEXT("derivative_valid"),TEXT("vorticity_x"),TEXT("vorticity_y"),TEXT("vorticity_z"),TEXT("vorticity_magnitude"),TEXT("q"),TEXT("divergence"),TEXT("helicity")};
        for(const auto& K:Keys)Add(K,K==TEXT("derivative_valid")?TEXT("1 only where complete original one-node halo is liquid/nonsolid; 0 is unavailable"):TEXT("Original-grid central differences of velocity; complete one-node halo liquid/nonsolid, derivative_valid required"));
        int32 Row=0;for(int32 Z=S.Minimum.Z;Z<S.Maximum.Z;Z+=R.PreviewStride)for(int32 Y=S.Minimum.Y;Y<S.Maximum.Y;Y+=R.PreviewStride)for(int32 X=S.Minimum.X;X<S.Maximum.X;X+=R.PreviewStride,++Row)
        {
            if((Row&4095)==0&&Cancelled(Cancel))return false;
            if(X<1||Y<1||Z<1||X>=S.Original.X-1||Y>=S.Original.Y-1||Z>=S.Original.Z-1)continue;bool Good=true;
            for(int32 DZ=-1;DZ<=1;++DZ)for(int32 DY=-1;DY<=1;++DY)for(int32 DX=-1;DX<=1;++DX)
            {
                int32 I=Node(FIntVector(X+DX,Y+DY,Z+DZ),S.Original);
                Good&=S.Core[TEXT("solid")][I]==0&&S.Core[TEXT("phi")][I]>=R.Mapping.LiquidMinimum;
            }
            if(!Good)continue;
            const FIntVector P(X,Y,Z);double G[3][3];for(int32 C=0;C<3;++C)for(int32 Axis=0;Axis<3;++Axis)
            {FIntVector D(0,0,0);D[Axis]=1;G[C][Axis]=(S.Core[Core[C]][Node(P+D,S.Original)]-S.Core[Core[C]][Node(P-D,S.Original)])/(2*S.Spacing[Axis]);}
            const FVector W(G[2][1]-G[1][2],G[0][2]-G[2][0],G[1][0]-G[0][1]);double SS=0,OO=0;
            for(int32 I=0;I<3;++I)for(int32 J=0;J<3;++J){SS+=FMath::Square((G[I][J]+G[J][I])*.5);OO+=FMath::Square((G[I][J]-G[J][I])*.5);}
            S.Fields[Keys[0]][Row]=1;S.Fields[Keys[1]][Row]=W.X;S.Fields[Keys[2]][Row]=W.Y;S.Fields[Keys[3]][Row]=W.Z;S.Fields[Keys[4]][Row]=W.Length();S.Fields[Keys[5]][Row]=.5*(OO-SS);S.Fields[Keys[6]][Row]=G[0][0]+G[1][1]+G[2][2];
            S.Fields[Keys[7]][Row]=S.Fields[TEXT("ux")][Row]*W.X+S.Fields[TEXT("uy")][Row]*W.Y+S.Fields[TEXT("uz")][Row]*W.Z;
        }
    }
    if(!R.PressureConvention.IsEmpty())
    {
        if(R.PressureConvention!=TEXT("wb_lattice")||R.Mapping.VelocityUnits!=TEXT("lattice")||!S.Fields.Contains(TEXT("rho"))||!S.Fields.Contains(TEXT("p_star"))||!S.Fields.Contains(TEXT("Pi_h"))||!S.Fields.Contains(TEXT("Pi_h0")))
        {E=TEXT("WB lattice pressure requires explicit convention, lattice state and rho/p_star/Pi_h/Pi_h0.");return false;}
        Add(TEXT("pressure"),TEXT("rho*(1/3)*p_star + Pi_h - Pi_h0; explicitly declared WB lattice convention"));
        for(int32 I=0;I<SelectedCount;++I)S.Fields[TEXT("pressure")][I]=S.Fields[TEXT("rho")][I]/3*S.Fields[TEXT("p_star")][I]+S.Fields[TEXT("Pi_h")][I]-S.Fields[TEXT("Pi_h0")][I];
    }
    const TArray<FString> Strain={TEXT("Sxx"),TEXT("Syy"),TEXT("Szz"),TEXT("Sxy"),TEXT("Sxz"),TEXT("Syz")};bool Diss=S.Fields.Contains(TEXT("nu"));for(const auto& K:Strain)Diss&=S.Fields.Contains(K);
    if(Diss){Add(TEXT("dissipation"),TEXT("2*nu*(Sxx^2+Syy^2+Szz^2+2*(Sxy^2+Sxz^2+Syz^2)) from stored state"));for(int32 I=0;I<SelectedCount;++I){if(S.Fields[TEXT("nu")][I]<0){E=TEXT("Stored viscosity must be nonnegative.");return false;}double Sum=0;for(int32 K=0;K<6;++K)Sum+=FMath::Square(S.Fields[Strain[K]][I])*(K<3?1:2);S.Fields[TEXT("dissipation")][I]=2*S.Fields[TEXT("nu")][I]*Sum;}}
    const bool Physical=R.Output==EStudioHome4ArchiveOutput::Recording||R.bPhysicalVTI;
    for(auto& Pair:S.Fields)
    {
        FString Label;double Scale;if(!Unit(Pair.Key,R.Mapping,Physical,Declared,Label,Scale,E))return false;S.Units.Add(Pair.Key,Label);
        for(double& V:Pair.Value){V*=Scale;if(!FMath::IsFinite(V)){E=TEXT("Original/derived field conversion overflows: ")+Pair.Key;return false;}}
    }
    S.Core.Empty();return !Cancelled(Cancel);
}
}

bool FStudioHome4ArchiveMapping::Validate(FString& E,bool Physical)const
{
    E=TEXT("Confirm original array axes, origin/spacing order and coordinate/velocity units.");
    if(!TArray<FString>{TEXT("xyz"),TEXT("xzy"),TEXT("yxz"),TEXT("yzx"),TEXT("zxy"),TEXT("zyx")}.Contains(AxisOrder)||(MetadataOrder!=TEXT("xyz")&&MetadataOrder!=TEXT("array"))||(CoordinateUnits!=TEXT("lattice")&&CoordinateUnits!=TEXT("physical"))||(VelocityUnits!=TEXT("lattice")&&VelocityUnits!=TEXT("physical")))return false;
    for(const auto& V:{DxMeters,DtSeconds,DensityReferenceKgM3})if(V&&(!FMath::IsFinite(V.GetValue())||V.GetValue()<=0)){E=TEXT("Original unit anchors must be finite positive.");return false;}
    if(!FMath::IsFinite(LiquidMinimum)||LiquidMinimum<0||LiquidMinimum>1||!FMath::IsFinite(TimeOriginSeconds)||TimeOriginSeconds<0){E=TEXT("Original liquid threshold must be 0–1; time origin must be finite nonnegative.");return false;}
    if(Physical&&(!DtSeconds||((CoordinateUnits==TEXT("lattice")||VelocityUnits==TEXT("lattice"))&&!DxMeters))){E=TEXT("Physical import requires original seconds/step, and metres/cell for lattice coordinates or velocity.");return false;}E.Empty();return true;
}
TSharedRef<FJsonObject> FStudioHome4ArchiveMapping::ToJSON()const
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("axisOrder"),AxisOrder);O->SetStringField(TEXT("metadataOrder"),MetadataOrder);O->SetStringField(TEXT("coordinateUnits"),CoordinateUnits);O->SetStringField(TEXT("velocityUnits"),VelocityUnits);
    StudioHome4ArchivePrivate::Optional(*O,TEXT("dxMeters"),DxMeters);StudioHome4ArchivePrivate::Optional(*O,TEXT("dtSeconds"),DtSeconds);StudioHome4ArchivePrivate::Optional(*O,TEXT("densityReferenceKgM3"),DensityReferenceKgM3);O->SetNumberField(TEXT("phiLiquidMin"),LiquidMinimum);O->SetNumberField(TEXT("timeOriginSeconds"),TimeOriginSeconds);return O;
}
bool StudioHome4Archives::MappingFromMetadata(const FJsonObject& Spec,FStudioHome4ArchiveMapping& Out,FString& E)
{
    using namespace StudioHome4ArchivePrivate;auto Candidate=Out;const FJsonObject* A=nullptr,*U=nullptr;
    if(!Object(Spec,TEXT("archive"),A,E)||!Object(Spec,TEXT("units"),U,E))return false;
    if(A)
    {
        if(!ContractString(*A,TEXT("axisOrder"),Candidate.AxisOrder,E)||!ContractString(*A,TEXT("metadataOrder"),Candidate.MetadataOrder,E)||!ContractString(*A,TEXT("coordinateUnits"),Candidate.CoordinateUnits,E)||!ContractString(*A,TEXT("velocityUnits"),Candidate.VelocityUnits,E))return false;
        for(auto P:{TPair<const TCHAR*,double*>(TEXT("phiLiquidMin"),&Candidate.LiquidMinimum),TPair<const TCHAR*,double*>(TEXT("timeOriginSeconds"),&Candidate.TimeOriginSeconds)})
            if(A->HasField(P.Key)&&(!A->TryGetNumberField(P.Key,*P.Value)||!FMath::IsFinite(*P.Value))){E=TEXT("Invalid original archive threshold/time origin.");return false;}
    }
    if(U&&(!MetadataNumber(*U,TEXT("dxMeters"),Candidate.DxMeters,E)||!MetadataNumber(*U,TEXT("dtSeconds"),Candidate.DtSeconds,E)||!MetadataNumber(*U,TEXT("densityReferenceKgM3"),Candidate.DensityReferenceKgM3,E)))return false;
    Out=MoveTemp(Candidate);E.Empty();return true;
}
FStudioHome4ArchiveInspection StudioHome4Archives::Inspect(const FString& Path,const FStudioLoadCancellation& C)
{
    using namespace StudioHome4ArchivePrivate;FStudioHome4ArchiveInspection R;R.Source.Path=FPaths::ConvertRelativePathToFull(Path);FStudioFileAccess Access(R.Source.Path);
    if(IFileManager::Get().FileSize(*R.Source.Path)>MaximumArchiveBytes||!Hash(R.Source.Path,R.Source.SHA256,C,R.Error))return R;
    FNpzReader Reader;if(!Reader.Open(R.Source.Path,C,R.Error)||!Reader.Scan(R.Error)){R.bCancelled=Cancelled(C);return R;}R.Members=Reader.Members();
    if(R.Members.ContainsByPredicate([](const auto& M){return M.Name==TEXT("run_spec");})){FNumericArray A;if(!Reader.Read(TEXT("run_spec"),A,R.Error,false)||!JSON(A.StringValue,R.OriginalRunSpec,R.Error))return R;}
    FString End;if(!Hash(R.Source.Path,End,C,R.Error)||End!=R.Source.SHA256){R.Error=TEXT("Source archive changed during inspection.");return R;}R.bCancelled=Cancelled(C);return R;
}

namespace StudioHome4ArchivePrivate
{
struct FStage
{FString Path;~FStage(){if(!Path.IsEmpty())IFileManager::Get().DeleteDirectory(*Path,false,true);}};
bool WriteText(const FString& Path,const FString& Text,int64 Limit,FString& E)
{
    FTCHARToUTF8 Bytes(*Text);if(Bytes.Length()>Limit){E=TEXT("Serialized provenance/metadata exceeds the native reader budget.");return false;}
    TUniquePtr<FArchive> F(IFileManager::Get().CreateFileWriter(*Path,FILEWRITE_NoReplaceExisting));
    if(!F){E=TEXT("Could not create a private staged file.");return false;}F->Serialize(const_cast<ANSICHAR*>(Bytes.Get()),Bytes.Length());
    if(F->IsError()||!F->Close()){E=TEXT("Could not finish a staged file. Check free space.");return false;}return true;
}
bool WriteJSON(const FString& Path,const TSharedRef<FJsonObject>& O,int64 Limit,FString& E)
{return WriteText(Path,Serialize(O)+TEXT("\n"),Limit,E);}
bool FileCopy(const FStudioHome4ArchiveSource& S,const FString& Destination,const FStudioLoadCancellation& C,FString& E)
{
    FStudioFileAccess Access(S.Path);const auto Before=IFileManager::Get().GetTimeStamp(*S.Path);
    TUniquePtr<FArchive> In(IFileManager::Get().CreateFileReader(*S.Path,FILEREAD_Silent));
    TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*Destination,FILEWRITE_NoReplaceExisting));
    if(!In||!Out||In->TotalSize()<=0||In->TotalSize()>StudioHome4Archives::MaximumArchiveBytes){E=TEXT("Source archive is missing, inaccessible or too large.");return false;}
    const int64 Size=In->TotalSize();uint8 Buffer[65536];for(int64 At=0;At<Size;)
    {if(Cancelled(C))return false;int64 N=FMath::Min<int64>(sizeof(Buffer),Size-At);In->Serialize(Buffer,N);Out->Serialize(Buffer,N);if(In->IsError()||Out->IsError()){E=TEXT("Could not pin the original source archive.");return false;}At+=N;}
    if(!Out->Close()||Out->IsError()||Size!=IFileManager::Get().FileSize(*S.Path)||Before!=IFileManager::Get().GetTimeStamp(*S.Path)){E=TEXT("Source archive changed while copied.");return false;}Out.Reset();In.Reset();
    FString SHA;if(!Hash(Destination,SHA,C,E)||SHA!=S.SHA256){E=TEXT("Source archive differs from its inspected SHA-256. Inspect it again.");return false;}return true;
}
struct FFieldWrite
{
    FString ID,Unit,Expression;bool bDerived=false,bAir=false,bHasDisplay=false;
    double Min=MAX_dbl,Max=-MAX_dbl,DisplayMin=0,DisplayMax=0;int64 Clips=0;
    TArray<uint32> CRC;TUniquePtr<FArchive> File;
};
bool WriteValues(FArchive& File,const TArray<double>& V,const FStudioLoadCancellation& C,uint32* CRC,FString& E)
{
    uint32 Check=0;uint8 B[65536];for(int32 At=0;At<V.Num();)
    {
        if(Cancelled(C))return false;const int32 N=FMath::Min(8192,V.Num()-At);
        for(int32 I=0;I<N;++I){uint64 Bits;FMemory::Memcpy(&Bits,&V[At+I],8);for(int32 K=0;K<8;++K)B[8*I+K]=uint8(Bits>>(8*K));}
        File.Serialize(B,N*8);if(File.IsError()){E=TEXT("Could not write original field values. Check free space.");return false;}
        if(CRC)Check=FCrc::MemCrc32(B,N*8,Check);At+=N;
    }
    if(CRC)*CRC=Check;return true;
}
void UInt64(FArchive& File,uint64 V){uint8 B[8];for(int32 I=0;I<8;++I)B[I]=uint8(V>>(8*I));File.Serialize(B,8);}
bool ArrayDescriptor(const FString& Folder,const FString& Name,const TArray<int64>& Shape,const FStudioLoadCancellation& C,TSharedPtr<FJsonObject>& Out,FString& E)
{
    FString SHA;if(!Hash(Folder/Name,SHA,C,E))return false;auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("path"),Name);O->SetStringField(TEXT("dtype"),TEXT("float64"));O->SetStringField(TEXT("byteOrder"),TEXT("little"));O->SetStringField(TEXT("sha256"),SHA);
    TArray<TSharedPtr<FJsonValue>> D;int64 Bytes=8;for(int64 N:Shape){Bytes*=N;D.Add(MakeShared<FJsonValueNumber>(double(N)));}O->SetArrayField(TEXT("shape"),D);O->SetNumberField(TEXT("byteLength"),double(Bytes));
    if(IFileManager::Get().FileSize(*(Folder/Name))!=Bytes){E=TEXT("Written original array has an incomplete byte count.");return false;}Out=O;return true;
}
FString XMLText(FString S)
{S.ReplaceInline(TEXT("&"),TEXT("&amp;"));S.ReplaceInline(TEXT("<"),TEXT("&lt;"));S.ReplaceInline(TEXT(">"),TEXT("&gt;"));S.ReplaceInline(TEXT("\""),TEXT("&quot;"));S.ReplaceInline(TEXT("'"),TEXT("&apos;"));return S;}
bool WriteVTI(const FString& Path,const FSnapshot& S,const FStudioHome4ArchiveRequest& R,const TArray<FString>& IDs,const FStudioLoadCancellation& C,FString& E)
{
    const bool Physical=R.bPhysicalVTI;const double Scale=Physical&&R.Mapping.CoordinateUnits==TEXT("lattice")?R.Mapping.DxMeters.GetValue():1.;
    const FVector Origin=(S.Origin+FVector(S.Minimum)*S.Spacing)*Scale,Spacing=S.Spacing*(R.PreviewStride*Scale);
    for(int32 Axis=0;Axis<3;++Axis)if(!FMath::IsFinite(Origin[Axis])||!FMath::IsFinite(Spacing[Axis])||Spacing[Axis]<=0||!FMath::IsFinite(Origin[Axis]+Spacing[Axis]*(S.Selected[Axis]-1)))
    {E=TEXT("Declared VTI affine coordinate conversion overflows.");return false;}
    FString Extent=FString::Printf(TEXT("0 %d 0 %d 0 %d"),S.Selected.X-1,S.Selected.Y-1,S.Selected.Z-1);
    FString Text=TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"ImageData\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\"><ImageData WholeExtent=\"")+Extent+
        FString::Printf(TEXT("\" Origin=\"%.17g %.17g %.17g\" Spacing=\"%.17g %.17g %.17g\"><Piece Extent=\""),Origin.X,Origin.Y,Origin.Z,Spacing.X,Spacing.Y,Spacing.Z)+Extent+TEXT("\"><PointData Scalars=\"phi\" Vectors=\"velocity\">");
    const uint64 Bytes=uint64(Count(S.Selected))*8;uint64 Offset=0;
    for(const auto& K:IDs){Text+=TEXT("<DataArray type=\"Float64\" Name=\"")+XMLText(K)+FString::Printf(TEXT("\" NumberOfComponents=\"1\" format=\"appended\" offset=\"%llu\"/>"),Offset);Offset+=8+Bytes;}
    Text+=FString::Printf(TEXT("<DataArray type=\"Float64\" Name=\"velocity\" NumberOfComponents=\"3\" format=\"appended\" offset=\"%llu\"/></PointData><CellData/></Piece></ImageData><AppendedData encoding=\"raw\">_"),Offset);
    TUniquePtr<FArchive> F(IFileManager::Get().CreateFileWriter(*Path,FILEWRITE_NoReplaceExisting));if(!F){E=TEXT("Could not create VTI stage.");return false;}FTCHARToUTF8 Header(*Text);F->Serialize(const_cast<ANSICHAR*>(Header.Get()),Header.Length());
    for(const auto& K:IDs){UInt64(*F,Bytes);if(!WriteValues(*F,S.Fields[K],C,nullptr,E))return false;}
    UInt64(*F,Bytes*3);TArray<double> Chunk;Chunk.Reserve(8190);
    for(int32 I=0;I<Count(S.Selected);++I){Chunk.Add(S.Fields[TEXT("ux")][I]);Chunk.Add(S.Fields[TEXT("uy")][I]);Chunk.Add(S.Fields[TEXT("uz")][I]);if(Chunk.Num()==8190||I+1==Count(S.Selected)){if(!WriteValues(*F,Chunk,C,nullptr,E))return false;Chunk.Reset();}}
    const char Tail[]="</AppendedData></VTKFile>\n";F->Serialize(const_cast<char*>(Tail),sizeof(Tail)-1);return !F->IsError()&&F->Close();
}
FStudioHome4ArchiveResult ConvertImpl(const FStudioHome4ArchiveRequest& R,const FStudioLoadCancellation& C,const TSharedPtr<FStudioHome4ArchiveWork,ESPMode::ThreadSafe>& Work)
{
    FStudioHome4ArchiveResult Out;FStage Stage;auto Fail=[&](FString E)
    {Out.bCancelled=Cancelled(C);Out.Error=Out.bCancelled?TEXT("Archive operation cancelled. Destination unchanged."):MoveTemp(E);if(Work)Work->State.store(EStudioHome4ArchiveState::Complete);return Out;};
    FString E;if(!R.Mapping.Validate(E,R.Output==EStudioHome4ArchiveOutput::Recording||R.bPhysicalVTI))return Fail(E);
    if(R.Sources.IsEmpty()||R.Sources.Num()>StudioHome4Archives::MaximumFrames||R.PreviewStride<1||R.PreviewStride>1024||R.CropMinimum.IsSet()!=R.CropMaximum.IsSet())return Fail(TEXT("Choose a bounded original snapshot list and a valid crop/stride."));
    if(!CleanText(R.Title,256)||!CleanText(R.SourceURI,2048)||!CleanText(R.Attribution,16384,true)||!CleanText(R.FolderName,128)||R.FolderName.StartsWith(TEXT("."))||R.FolderName.Contains(TEXT("/"))||R.FolderName.Contains(TEXT("\\"))||R.FolderName.Contains(TEXT(":"))||R.FolderName!=R.FolderName.TrimStartAndEnd())return Fail(TEXT("Provide a source URI, attribution, title and a new plain output folder name."));
    if(FPaths::IsRelative(R.OutputParent)||!IFileManager::Get().DirectoryExists(*R.OutputParent))return Fail(TEXT("Choose an existing absolute output parent folder."));
    int64 Inputs=0;for(const auto& S:R.Sources)
    {const int64 N=IFileManager::Get().FileSize(*S.Path);if(N<=0||N>StudioHome4Archives::MaximumArchiveBytes||Inputs>StudioHome4Archives::MaximumJobInputBytes-N||S.SHA256.Len()!=64)return Fail(TEXT("Source list exceeds compressed input limits or lacks an inspected SHA-256."));Inputs+=N;}
    FStudioFileAccess Access(R.OutputParent);if(!StudioFileDialog::CreateExportStage(R.OutputParent,Stage.Path,E))return Fail(E);Out.Path=R.OutputParent/R.FolderName;
    TArray<TSharedPtr<FJsonValue>> Snapshots,Frames;TArray<FFieldWrite> Writers;TArray<FString> IDs;FSnapshot First;FBox EmittedBounds(ForceInit);
    int64 OutputBytes=0,ExpandedBytes=0,ManifestBytes=128;int32 Previous=-1;double PreviousTime=-MAX_dbl;
    const FString TimeUnit=R.Mapping.DtSeconds?TEXT("s"):TEXT("solver_step");
    FString PVD=TEXT("<?xml version=\"1.0\"?><VTKFile type=\"Collection\" version=\"0.1\" byte_order=\"LittleEndian\"><!-- timestep unit: ")+TimeUnit+TEXT("; original source coordinates, see provenance.json --><Collection>");
    for(int32 Ordinal=0;Ordinal<R.Sources.Num();++Ordinal)
    {
        if(Cancelled(C))return Fail({});const FString Copy=Stage.Path/TEXT("original.npz");if(!FileCopy(R.Sources[Ordinal],Copy,C,E))return Fail(E);
        FSnapshot S;TArray<FStudioHome4ArchiveMember> Members;
        {FNpzReader Reader;if(!Reader.Open(Copy,C,E)||!Reader.Scan(E))return Fail(E);Members=Reader.Members();for(const auto& M:Members){if(ExpandedBytes>StudioHome4Archives::MaximumJobInputBytes-M.PayloadBytes)return Fail(TEXT("Snapshot list exceeds total expanded input limits."));ExpandedBytes+=M.PayloadBytes;}if(!Snapshot(Reader,R,S,E,C))return Fail(E);}
        IFileManager::Get().Delete(*Copy,false,true);
        if(S.Iteration<=Previous||S.Time<=PreviousTime)return Fail(TEXT("Supply snapshots in strictly increasing original solver-step/time order."));Previous=S.Iteration;PreviousTime=S.Time;
        TArray<FString> CurrentIDs;S.Fields.GetKeys(CurrentIDs);CurrentIDs.Sort();
        if(Ordinal==0)
        {
            IDs=CurrentIDs;First.Original=S.Original;First.Minimum=S.Minimum;First.Maximum=S.Maximum;First.Selected=S.Selected;
            First.Origin=S.Origin;First.Spacing=S.Spacing;First.RunSpec=S.RunSpec;First.RunIdentity=S.RunIdentity;First.Units=S.Units;
            const int64 PerFrame=Count(S.Selected)*8*IDs.Num();
            const int64 Estimate=PerFrame*R.Sources.Num()+Count(S.Selected)*32+16LL*1024*1024+(R.Output==EStudioHome4ArchiveOutput::VTI?Count(S.Selected)*24*R.Sources.Num():0);
            if(Estimate>StudioHome4Archives::MaximumOutputBytes)return Fail(TEXT("Output exceeds the bounded 64 GiB job limit. Reduce frames/crop/stride."));
            uint64 TotalDisk=0,FreeDisk=0;if(FPlatformMisc::GetDiskTotalAndFreeSpace(R.OutputParent,TotalDisk,FreeDisk)&&FreeDisk<uint64(Estimate+StudioHome4Archives::MaximumArchiveBytes))return Fail(TEXT("Insufficient free space for the recording and private source copy."));
            if(R.Output==EStudioHome4ArchiveOutput::Recording)
            {
                TArray<double> Coordinates;Coordinates.Reserve(int32(Count(S.Selected))*3);TUniquePtr<FArchive> IdFile(IFileManager::Get().CreateFileWriter(*(Stage.Path/TEXT("point-ids.i64")),FILEWRITE_NoReplaceExisting));if(!IdFile)return Fail(TEXT("Could not stage original point IDs."));
                const double Scale=R.Mapping.CoordinateUnits==TEXT("lattice")?R.Mapping.DxMeters.GetValue():1.;
                for(int32 Z=S.Minimum.Z;Z<S.Maximum.Z;Z+=R.PreviewStride)for(int32 Y=S.Minimum.Y;Y<S.Maximum.Y;Y+=R.PreviewStride)for(int32 X=S.Minimum.X;X<S.Maximum.X;X+=R.PreviewStride)
                {const FVector P=(S.Origin+FVector(X,Y,Z)*S.Spacing)*Scale;for(int32 A=0;A<3;++A){if(!FMath::IsFinite(P[A])||FMath::Abs(P[A])>1.e8)return Fail(TEXT("Native physical coordinates exceed renderer limits."));Coordinates.Add(P[A]);}EmittedBounds+=P;UInt64(*IdFile,uint64(Node(FIntVector(X,Y,Z),S.Original)));}
                if(!IdFile->Close()||IdFile->IsError())return Fail(TEXT("Could not finish original point IDs."));IdFile.Reset();TUniquePtr<FArchive> CF(IFileManager::Get().CreateFileWriter(*(Stage.Path/TEXT("coordinates.f64")),FILEWRITE_NoReplaceExisting));if(!CF||!WriteValues(*CF,Coordinates,C,nullptr,E)||!CF->Close())return Fail(E);Coordinates.Empty();CF.Reset();
                for(const auto& K:IDs)
                {
                    FFieldWrite W;W.ID=K;W.Unit=S.Units[K];W.bDerived=S.Derived.Contains(K);W.Expression=S.Expressions.FindRef(K);W.bAir=AirMasked(K);
                    // Range policy uses raw original source masks, independent of viewer toggles.
                    TArray<double> Available;for(int32 I=0;I<S.Fields[K].Num();++I)if(S.Fields[TEXT("solid")][I]==0&&(!W.bAir||S.Fields[TEXT("phi")][I]>=R.Mapping.LiquidMinimum)&&(!DerivedField(K)||S.Fields[TEXT("derivative_valid")][I]>0))Available.Add(S.Fields[K][I]);
                    if(!Available.IsEmpty()){Available.Sort();W.bHasDisplay=true;W.DisplayMin=Available[0];double At=.99*(Available.Num()-1);int32 Lo=FMath::FloorToInt(At),Hi=FMath::CeilToInt(At);W.DisplayMax=Available[Lo]+(Available[Hi]-Available[Lo])*(At-Lo);for(double V:Available)W.Clips+=V>W.DisplayMax;}
                    W.File.Reset(IFileManager::Get().CreateFileWriter(*(Stage.Path/(K+TEXT(".f64"))),FILEWRITE_NoReplaceExisting));if(!W.File)return Fail(TEXT("Could not create a native original field file."));Writers.Add(MoveTemp(W));
                }
            }
        }
        else if(CurrentIDs!=IDs||S.Original!=First.Original||S.Selected!=First.Selected||S.Origin!=First.Origin||S.Spacing!=First.Spacing||S.RunIdentity!=First.RunIdentity||S.Units.OrderIndependentCompareEqual(First.Units)==false)return Fail(TEXT("Original geometry, fields, run identity or unit/reference map changed across snapshots."));
        if(R.Output==EStudioHome4ArchiveOutput::Recording)for(auto& W:Writers)
        {uint32 CRC;const auto& V=S.Fields[W.ID];if(!WriteValues(*W.File,V,C,&CRC,E))return Fail(E);W.CRC.Add(CRC);for(double D:V){W.Min=FMath::Min(W.Min,D);W.Max=FMath::Max(W.Max,D);}OutputBytes+=V.Num()*8;}
        else
        {const FString Name=FString::Printf(TEXT("snapshot-%010d.vti"),S.Iteration);if(!WriteVTI(Stage.Path/Name,S,R,IDs,C,E))return Fail(E);OutputBytes+=IFileManager::Get().FileSize(*(Stage.Path/Name));PVD+=FString::Printf(TEXT("<DataSet timestep=\"%.17g\" group=\"\" part=\"0\" file=\"%s\"/>"),S.Time,*XMLText(Name));}
        auto Frame=MakeShared<FJsonObject>();Frame->SetNumberField(TEXT("index"),S.Iteration);Frame->SetNumberField(TEXT("time"),S.Time);Frame->SetStringField(TEXT("label"),FString::Printf(TEXT("step_%d"),S.Iteration));Frames.Add(MakeShared<FJsonValueObject>(Frame));
        auto Source=MakeShared<FJsonObject>();Source->SetStringField(TEXT("source"),R.Sources[Ordinal].Path);Source->SetStringField(TEXT("sourceSHA256"),R.Sources[Ordinal].SHA256);Source->SetNumberField(TEXT("iteration"),S.Iteration);Optional(*Source,TEXT("timePhysicalSeconds"),R.Mapping.DtSeconds?TOptional<double>(S.Time):TOptional<double>());Source->SetNumberField(TEXT("timeValue"),S.Time);Source->SetStringField(TEXT("timeUnit"),TimeUnit);Source->SetArrayField(TEXT("originalDimensionsXYZ"),Triple(S.Original));Source->SetArrayField(TEXT("originalOriginXYZ"),Triple(S.Origin));Source->SetArrayField(TEXT("originalSpacingXYZ"),Triple(S.Spacing));if(S.RunSpec)Source->SetObjectField(TEXT("runSpec"),S.RunSpec);
        TArray<TSharedPtr<FJsonValue>> Headers;for(const auto& M:Members){auto H=MakeShared<FJsonObject>();H->SetStringField(TEXT("name"),M.Name);H->SetStringField(TEXT("dtype"),M.DType);H->SetBoolField(TEXT("fortranOrder"),M.bFortran);TArray<TSharedPtr<FJsonValue>> Shape;for(int64 N:M.Shape)Shape.Add(MakeShared<FJsonValueNumber>(double(N)));H->SetArrayField(TEXT("shape"),Shape);H->SetBoolField(TEXT("retained"),S.Fields.Contains(M.Name)||M.Name==TEXT("iteration")||M.Name==TEXT("origin")||M.Name==TEXT("spacing")||M.Name==TEXT("run_spec"));Headers.Add(MakeShared<FJsonValueObject>(H));}Source->SetArrayField(TEXT("members"),Headers);
        auto FieldInfo=MakeShared<FJsonObject>();for(const auto& K:IDs){auto Info=MakeShared<FJsonObject>();Info->SetStringField(TEXT("unit"),S.Units[K]);Info->SetStringField(TEXT("origin"),S.Derived.Contains(K)?TEXT("derived"):TEXT("source"));FieldInfo->SetObjectField(K,Info);}Source->SetObjectField(TEXT("fields"),FieldInfo);
        if(!ReserveManifestBytes(Serialize(Source),ManifestBytes))return Fail(TEXT("Original source manifest exceeds cumulative UTF-8 metadata bounds. Reduce the frame list."));
        Snapshots.Add(MakeShared<FJsonValueObject>(Source));
        if(Work){Work->Completed.store(Ordinal+1);Work->Bytes.store(OutputBytes);}Out.Frames=Ordinal+1;
    }
    auto Manifest=MakeShared<FJsonObject>();Manifest->SetNumberField(TEXT("version"),1);Manifest->SetStringField(TEXT("kind"),TEXT("home4_source_snapshot_manifest"));Manifest->SetArrayField(TEXT("snapshots"),Snapshots);
    if(!WriteJSON(Stage.Path/TEXT("source-manifest.json"),Manifest,8LL*1024*1024,E))return Fail(E);FString ManifestSHA;if(!Hash(Stage.Path/TEXT("source-manifest.json"),ManifestSHA,C,E))return Fail(E);
    auto ManifestRef=MakeShared<FJsonObject>();ManifestRef->SetStringField(TEXT("path"),TEXT("source-manifest.json"));ManifestRef->SetStringField(TEXT("sha256"),ManifestSHA);ManifestRef->SetNumberField(TEXT("frameCount"),R.Sources.Num());
    auto Provenance=MakeShared<FJsonObject>();Provenance->SetNumberField(TEXT("version"),1);Provenance->SetStringField(TEXT("kind"),TEXT("home4_native_archive_conversion"));Provenance->SetStringField(TEXT("converter"),TEXT("LBMStudio native HOME4 archive importer v1"));Provenance->SetObjectField(TEXT("mapping"),R.Mapping.ToJSON());Provenance->SetObjectField(TEXT("sourcesManifest"),ManifestRef);Provenance->SetStringField(TEXT("sourceURL"),R.SourceURI);Provenance->SetArrayField(TEXT("cropMinimumXYZ"),Triple(First.Minimum));Provenance->SetArrayField(TEXT("cropMaximumXYZ"),Triple(First.Maximum));Provenance->SetNumberField(TEXT("previewStride"),R.PreviewStride);Provenance->SetStringField(TEXT("pressureConvention"),R.PressureConvention);Provenance->SetBoolField(TEXT("derivatives"),R.bDerivatives);Provenance->SetStringField(TEXT("pointIdDefinition"),TEXT("original x + Nx*(y+Ny*z), before crop/stride"));Provenance->SetStringField(TEXT("changes"),TEXT("Explicit axis/storage permutation, crop/preview selection, declared unit conversion, float64 storage and separately identified derived fields. All source member retention is reported; no CFD computed."));
    Provenance->SetStringField(TEXT("timeUnit"),TimeUnit);Provenance->SetStringField(TEXT("timeDefinition"),R.Mapping.DtSeconds?TEXT("Original iteration * dtSeconds + timeOriginSeconds"):TEXT("Original solver iteration; physical time unavailable without dtSeconds"));
    if(!WriteJSON(Stage.Path/TEXT("provenance.json"),Provenance,256*1024,E)||!WriteText(Stage.Path/TEXT("ATTRIBUTION.txt"),R.Attribution+TEXT("\nSource: ")+R.SourceURI+TEXT("\n"),256*1024,E))return Fail(E);
    if(R.Output==EStudioHome4ArchiveOutput::Recording)
    {
        for(auto& W:Writers){if(!W.File->Close()||W.File->IsError())return Fail(TEXT("Could not close complete original field arrays."));W.File.Reset();}
        auto Meta=MakeShared<FJsonObject>();Meta->SetNumberField(TEXT("version"),3);Meta->SetStringField(TEXT("kind"),TEXT("field_recording"));
        FString ProvSHA,AttrSHA;if(!Hash(Stage.Path/TEXT("provenance.json"),ProvSHA,C,E)||!Hash(Stage.Path/TEXT("ATTRIBUTION.txt"),AttrSHA,C,E))return Fail(E);
        Meta->SetStringField(TEXT("id"),TEXT("HOME4_")+ProvSHA.Left(32));Meta->SetStringField(TEXT("title"),R.Title);Meta->SetStringField(TEXT("sourceURL"),R.SourceURI);Meta->SetNumberField(TEXT("spatialDimensions"),3);Meta->SetStringField(TEXT("coordinateUnit"),TEXT("m"));Meta->SetStringField(TEXT("timeUnit"),TEXT("s"));Meta->SetStringField(TEXT("timeOrigin"),TEXT("Original iteration * explicit dtSeconds + explicit timeOriginSeconds"));Meta->SetNumberField(TEXT("pointCount"),double(Count(First.Selected)));Meta->SetNumberField(TEXT("frameCount"),R.Sources.Num());Meta->SetStringField(TEXT("defaultScalar"),TEXT("speed"));Meta->SetStringField(TEXT("provenanceSHA256"),ProvSHA);Meta->SetStringField(TEXT("attributionSHA256"),AttrSHA);Meta->SetArrayField(TEXT("frames"),Frames);
        auto Topology=MakeShared<FJsonObject>();Topology->SetStringField(TEXT("kind"),TEXT("points"));Topology->SetStringField(TEXT("origin"),TEXT("source"));Topology->SetField(TEXT("connectivity"),MakeShared<FJsonValueNull>());Meta->SetObjectField(TEXT("topology"),Topology);
        TSharedPtr<FJsonObject> Coords;if(!ArrayDescriptor(Stage.Path,TEXT("coordinates.f64"),{Count(First.Selected),3},C,Coords,E))return Fail(E);Meta->SetObjectField(TEXT("coordinates"),Coords);
        auto Ids=MakeShared<FJsonObject>();FString IdHash;if(!Hash(Stage.Path/TEXT("point-ids.i64"),IdHash,C,E))return Fail(E);Ids->SetStringField(TEXT("path"),TEXT("point-ids.i64"));Ids->SetStringField(TEXT("dtype"),TEXT("int64"));Ids->SetStringField(TEXT("byteOrder"),TEXT("little"));Ids->SetStringField(TEXT("sha256"),IdHash);Ids->SetNumberField(TEXT("count"),double(Count(First.Selected)));Meta->SetObjectField(TEXT("pointIds"),Ids);
        const double Scale=R.Mapping.CoordinateUnits==TEXT("lattice")?R.Mapping.DxMeters.GetValue():1.;const FVector Origin=(First.Origin+FVector(First.Minimum)*First.Spacing)*Scale,Spacing=First.Spacing*(Scale*R.PreviewStride);
        // Bounds attest the exact original coordinate bytes; regrouping affine
        // arithmetic can produce a different last bit even for the same grid.
        auto Bounds=MakeShared<FJsonObject>();Bounds->SetArrayField(TEXT("min"),Triple(EmittedBounds.Min));Bounds->SetArrayField(TEXT("max"),Triple(EmittedBounds.Max));Meta->SetObjectField(TEXT("sourceBounds"),Bounds);
        TArray<TSharedPtr<FJsonValue>> Fields;
        auto Ranges=MakeShared<FJsonObject>();
        for(const auto& W:Writers)
        {
            auto F=MakeShared<FJsonObject>();F->SetStringField(TEXT("id"),W.ID);FString Label=W.ID;Label.ReplaceInline(TEXT("_"),TEXT(" "));F->SetStringField(TEXT("label"),Label);F->SetStringField(TEXT("unit"),W.Unit);F->SetStringField(TEXT("association"),TEXT("point"));F->SetStringField(TEXT("origin"),W.bDerived?TEXT("derived"):TEXT("source"));F->SetBoolField(TEXT("static"),false);F->SetBoolField(TEXT("airMaskDefault"),W.bAir);F->SetArrayField(TEXT("range"),Numbers({W.Min,W.Max}));if(W.bDerived)F->SetStringField(TEXT("expression"),W.Expression);
            if(DerivedField(W.ID))F->SetStringField(TEXT("validityMask"),TEXT("derivative_valid"));
            if(W.bHasDisplay){auto Range=MakeShared<FJsonObject>();Range->SetNumberField(TEXT("minimum"),W.DisplayMin);Range->SetNumberField(TEXT("maximum"),W.DisplayMax);Range->SetStringField(TEXT("policy"),TEXT("first_frame_percentile_99"));Range->SetNumberField(TEXT("clippedAbove"),double(W.Clips));F->SetObjectField(TEXT("displayRange"),Range);Ranges->SetObjectField(W.ID,Range);}
            if(W.ID==TEXT("ux")||W.ID==TEXT("uy")||W.ID==TEXT("uz")){F->SetStringField(TEXT("vector"),TEXT("velocity"));F->SetStringField(TEXT("component"),W.ID.Right(1));}
            TSharedPtr<FJsonObject> A;if(!ArrayDescriptor(Stage.Path,W.ID+TEXT(".f64"),{R.Sources.Num(),Count(First.Selected)},C,A,E))return Fail(E);TArray<TSharedPtr<FJsonValue>> Checks;for(uint32 V:W.CRC)Checks.Add(MakeShared<FJsonValueNumber>(double(V)));A->SetArrayField(TEXT("frameCRC32"),Checks);F->SetObjectField(TEXT("array"),A);Fields.Add(MakeShared<FJsonValueObject>(F));
        }
        Meta->SetArrayField(TEXT("fields"),Fields);if(!WriteJSON(Stage.Path/TEXT("ranges.json"),Ranges,256*1024,E))return Fail(E);
        auto G=MakeShared<FJsonObject>();G->SetNumberField(TEXT("version"),1);G->SetStringField(TEXT("kind"),TEXT("home4_structured_source"));G->SetStringField(TEXT("layout"),TEXT("x_fastest_node_grid"));G->SetArrayField(TEXT("dimensionsXYZ"),Triple(First.Selected));G->SetArrayField(TEXT("originalDimensionsXYZ"),Triple(First.Original));G->SetArrayField(TEXT("cropMinimumXYZ"),Triple(First.Minimum));G->SetArrayField(TEXT("cropMaximumXYZ"),Triple(First.Maximum));G->SetNumberField(TEXT("previewStride"),R.PreviewStride);G->SetArrayField(TEXT("originMeters"),Triple(Origin));G->SetArrayField(TEXT("spacingMeters"),Triple(Spacing));G->SetArrayField(TEXT("originalOriginXYZ"),Triple(First.Origin));G->SetArrayField(TEXT("originalSpacingXYZ"),Triple(First.Spacing));G->SetStringField(TEXT("axisOrder"),R.Mapping.AxisOrder);G->SetStringField(TEXT("metadataOrder"),R.Mapping.MetadataOrder);G->SetStringField(TEXT("coordinateUnits"),R.Mapping.CoordinateUnits);G->SetStringField(TEXT("velocityUnits"),R.Mapping.VelocityUnits);G->SetNumberField(TEXT("timeOriginSeconds"),R.Mapping.TimeOriginSeconds);G->SetNumberField(TEXT("phiLiquidMin"),R.Mapping.LiquidMinimum);G->SetStringField(TEXT("phaseField"),TEXT("phi"));G->SetStringField(TEXT("solidField"),TEXT("solid"));G->SetStringField(TEXT("solidSupportField"),TEXT("solid_support"));G->SetStringField(TEXT("liquidSupportField"),TEXT("liquid_support"));G->SetStringField(TEXT("derivativeValidityField"),R.bDerivatives?TEXT("derivative_valid"):TEXT(""));G->SetObjectField(TEXT("sourceManifest"),ManifestRef);
        auto U=MakeShared<FJsonObject>();Optional(*U,TEXT("dxMeters"),R.Mapping.DxMeters);Optional(*U,TEXT("dtSeconds"),R.Mapping.DtSeconds);Optional(*U,TEXT("densityReferenceKgM3"),R.Mapping.DensityReferenceKgM3);G->SetObjectField(TEXT("units"),U);
        auto Reference=MakeShared<FJsonObject>();const FJsonObject* OriginalRef=nullptr,*Fluids=nullptr;if(First.RunSpec&&(!Object(*First.RunSpec,TEXT("reference"),OriginalRef,E)||!Object(*First.RunSpec,TEXT("fluids"),Fluids,E)))return Fail(E);
        for(const TCHAR* K:{TEXT("lengthCells"),TEXT("speedCellsPerStep"),TEXT("timeSteps")}){TOptional<double> V;if(OriginalRef&&!MetadataNumber(*OriginalRef,K,V,E))return Fail(E);Optional(*Reference,K,V);}TOptional<double> Density;if(Fluids&&!MetadataNumber(*Fluids,TEXT("rhoHeavy"),Density,E))return Fail(E);Optional(*Reference,TEXT("densityLattice"),Density);G->SetObjectField(TEXT("reference"),Reference);
        if(First.RunSpec)for(auto P:{TPair<const TCHAR*,const TCHAR*>(TEXT("runId"),TEXT("sourceRunId")),TPair<const TCHAR*,const TCHAR*>(TEXT("recipeId"),TEXT("recipeId")),TPair<const TCHAR*,const TCHAR*>(TEXT("lineageId"),TEXT("lineageId"))})
        {FString V;if(First.RunSpec->HasField(P.Key)){if(!First.RunSpec->TryGetStringField(P.Key,V)||!CleanText(V,256))return Fail(TEXT("Invalid original run/recipe/lineage identity."));G->SetStringField(P.Value,V);}}
        Meta->SetObjectField(TEXT("structuredGrid"),G);TArray<TSharedPtr<FJsonValue>> Notes;for(const TCHAR* T:{TEXT("Original source snapshots imported; no solver execution or numerical validation."),TEXT("Original affine samples retained; explicit crop/stride is a preview, not full numerical output."),TEXT("Derived invalid nodes are unavailable and accompanied by derivative_valid."),TEXT("Unretained source members are explicitly listed in source-manifest.json; unknown normalization is never inferred.")})Notes.Add(MakeShared<FJsonValueString>(T));Meta->SetArrayField(TEXT("limitations"),Notes);
        if(!WriteJSON(Stage.Path/TEXT("recording.json"),Meta,8LL*1024*1024,E))return Fail(E);
        {auto Verified=StudioRecordings::Import(Stage.Path/TEXT("recording.json"),0,C);if(!Verified.Source)return Fail(Verified.Error);} // Release all stage-path readers before rename.
    }
    else if(!WriteText(Stage.Path/TEXT("sequence.pvd"),PVD+TEXT("</Collection></VTKFile>\n"),8LL*1024*1024,E))return Fail(E);
    if(Cancelled(C))return Fail({});if(Work){auto Expected=EStudioHome4ArchiveState::Working;if(!Work->State.compare_exchange_strong(Expected,EStudioHome4ArchiveState::Publishing)){C->store(true);return Fail({});}}
    if(!StudioFileDialog::PublishExportDirectory(Stage.Path,Out.Path,E))return Fail(E);Stage.Path.Empty();StudioFileDialog::RememberAccess(Out.Path);Out.bSuccess=true;
    if(R.Output==EStudioHome4ArchiveOutput::Recording)Out.RecordingJSON=Out.Path/TEXT("recording.json");if(Work)Work->State.store(EStudioHome4ArchiveState::Complete);return Out;
}
}
FStudioHome4ArchiveResult StudioHome4Archives::Convert(const FStudioHome4ArchiveRequest& R,const FStudioLoadCancellation& C)
{return StudioHome4ArchivePrivate::ConvertImpl(R,C,{});}
FStudioHome4ArchiveTask::~FStudioHome4ArchiveTask(){Cancel();}
bool FStudioHome4ArchiveTask::Start(FStudioHome4ArchiveRequest R,FString& E)
{
    if(Pending.IsValid()){E=TEXT("Wait for the current archive worker to finish or cancel it.");return false;}Work=MakeShared<FStudioHome4ArchiveWork,ESPMode::ThreadSafe>();Work->Total=R.Sources.Num();
    Pending=Async(EAsyncExecution::ThreadPool,[R=MoveTemp(R),W=Work]{return StudioHome4ArchivePrivate::ConvertImpl(R,W->Cancellation,W);});E.Empty();return true;
}
bool FStudioHome4ArchiveTask::Cancel()
{if(!Work)return false;auto Expected=EStudioHome4ArchiveState::Working;if(!Work->State.compare_exchange_strong(Expected,EStudioHome4ArchiveState::Cancelled))return false;Work->Cancellation->store(true);return true;}
bool FStudioHome4ArchiveTask::IsBusy()const{return Pending.IsValid();}
FStudioHome4ArchiveProgress FStudioHome4ArchiveTask::Progress()const
{FStudioHome4ArchiveProgress P;if(Work){P.State=Work->State.load();P.CompletedFrames=Work->Completed.load();P.TotalFrames=Work->Total;P.Bytes=Work->Bytes.load();}return P;}
TOptional<FStudioHome4ArchiveResult> FStudioHome4ArchiveTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};auto R=Pending.Get();Pending={};Work.Reset();return R;}
