#include "StudioMeshImport.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Math/RotationMatrix.h"
#include <cstdlib>
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
    bool Number(const FString& Text,double& Out)
    {
        if(Text.IsEmpty()) return false;
        const FTCHARToUTF8 Bytes(*Text);char* End=nullptr;Out=std::strtod(Bytes.Get(),&End);
        return End==Bytes.Get()+Bytes.Length() && FMath::IsFinite(Out) && FMath::Abs(Out)<=1.e8;
    }
    bool Vector(const TArray<FString>& T,int32 Offset,FVector& Out)
    { return T.Num()>=Offset+3 && Number(T[Offset],Out.X) && Number(T[Offset+1],Out.Y) && Number(T[Offset+2],Out.Z); }
    uint32 U32(const uint8* P)
    { return uint32(P[0])|(uint32(P[1])<<8)|(uint32(P[2])<<16)|(uint32(P[3])<<24); }
    double Float(const uint8* P)
    { uint32 Bits=U32(P); float V; FMemory::Memcpy(&V,&Bits,4); return V; }
    bool Index(const FString& Token,int32 Count,int32& Out)
    {
        if(Token.IsEmpty()) return false;
        int32 Start=Token[0]==TEXT('-')||Token[0]==TEXT('+')?1:0;
        if(Start==Token.Len()||Token.Len()>10) return false;
        int64 Value=0;
        for(int32 I=Start;I<Token.Len();++I) {if(!FChar::IsDigit(Token[I])) return false; Value=Value*10+Token[I]-TEXT('0');}
        if(Token[0]==TEXT('-')) Value=-Value;
        if(!Value) return false;
        Value=Value<0?Count+Value:Value-1;
        if(Value<0||Value>=Count) return false;
        Out=int32(Value);return true;
    }
    double Cross(const FVector2D& A,const FVector2D& B,const FVector2D& C)
    {return (B.X-A.X)*(C.Y-A.Y)-(B.Y-A.Y)*(C.X-A.X);}
    bool AddTriangle(FStudioImportedMesh& M,int32 A,int32 B,int32 C,int32 Patch,FString& Error)
    {
        if(M.Indices.Num()/3>=StudioMeshImport::MaxTriangles) {Error=TEXT("Mesh exceeds the 500,000-triangle import limit.");return false;}
        const FVector AB=M.Positions[B]-M.Positions[A],AC=M.Positions[C]-M.Positions[A];
        const double Scale=FMath::Max(AB.SizeSquared(),AC.SizeSquared());
        if(Scale==0||FVector::CrossProduct(AB,AC).SizeSquared()<=Scale*Scale*1.e-24)
        {Error=TEXT("Mesh contains a zero-area triangle. Repair the source before importing.");return false;}
        M.Indices.Append({A,B,C});M.TrianglePatches.Add(Patch);return true;
    }
    bool Polygon(FStudioImportedMesh& M,TArray<int32> Face,int32 Patch,FString& Error)
    {
        if(Face.Num()<3||Face.Num()>256) {Error=TEXT("OBJ faces must contain 3–256 vertices.");return false;}
        FVector Normal=FVector::ZeroVector;
        FBox Box(ForceInit);TSet<int32> Unique;
        for(int32 I=0;I<Face.Num();++I)
        {
            if(Unique.Contains(Face[I])) {Error=TEXT("OBJ face repeats a vertex.");return false;}Unique.Add(Face[I]);
            const auto& A=M.Positions[Face[I]];const auto& B=M.Positions[Face[(I+1)%Face.Num()]];
            Normal.X+=(A.Y-B.Y)*(A.Z+B.Z);Normal.Y+=(A.Z-B.Z)*(A.X+B.X);Normal.Z+=(A.X-B.X)*(A.Y+B.Y);Box+=A;
        }
        if(Normal.IsNearlyZero(1.e-20)) {Error=TEXT("OBJ face has zero area or crosses itself.");return false;}
        const double Extent=Box.GetSize().GetMax();const FVector Unit=Normal.GetSafeNormal(1.e-40);
        for(int32 I:Face) if(FMath::Abs(FVector::DotProduct(M.Positions[I]-M.Positions[Face[0]],Unit))>FMath::Max(1.e-12,Extent*1.e-6))
        {Error=TEXT("OBJ contains a nonplanar polygon. Triangulate that face in the source.");return false;}
        const FVector Abs=Normal.GetAbs();const int32 Drop=Abs.X>Abs.Y?(Abs.X>Abs.Z?0:2):(Abs.Y>Abs.Z?1:2);
        auto Project=[Drop](const FVector& V){return Drop==0?FVector2D(V.Y,V.Z):Drop==1?FVector2D(V.X,V.Z):FVector2D(V.X,V.Y);};
        TArray<FVector2D> P;for(int32 I:Face) P.Add(Project(M.Positions[I]));
        const double Eps=FMath::Max(1.e-28,Extent*Extent*1.e-12);double Area=0;
        for(int32 I=0;I<P.Num();++I) {const auto& A=P[I];const auto& B=P[(I+1)%P.Num()];Area+=A.X*B.Y-B.X*A.Y;}
        if(FMath::Abs(Area)<=Eps) {Error=TEXT("OBJ polygon has no usable area.");return false;}
        // Reject crossing edges before ear clipping; never silently fan a concave face.
        for(int32 I=0;I<P.Num();++I) for(int32 J=I+1;J<P.Num();++J)
        {
            const int32 NextI=(I+1)%P.Num(),NextJ=(J+1)%P.Num();if(NextI==J||NextJ==I) continue;
            const double A=Cross(P[I],P[NextI],P[J]),B=Cross(P[I],P[NextI],P[NextJ]),C=Cross(P[J],P[NextJ],P[I]),D=Cross(P[J],P[NextJ],P[NextI]);
            if(A*B<0&&C*D<0) {Error=TEXT("OBJ polygon edges cross. Repair the source face.");return false;}
        }
        const double Sign=Area>0?1.:-1.;TArray<int32> Remaining;for(int32 I=0;I<Face.Num();++I) Remaining.Add(I);
        while(Remaining.Num()>3)
        {
            bool Found=false;
            for(int32 I=0;I<Remaining.Num();++I)
            {
                const int32 A=Remaining[(I+Remaining.Num()-1)%Remaining.Num()],B=Remaining[I],C=Remaining[(I+1)%Remaining.Num()];
                if(Sign*Cross(P[A],P[B],P[C])<=Eps) continue;
                bool Inside=false;
                for(int32 Q:Remaining) if(Q!=A&&Q!=B&&Q!=C && Sign*Cross(P[A],P[B],P[Q])>=-Eps && Sign*Cross(P[B],P[C],P[Q])>=-Eps && Sign*Cross(P[C],P[A],P[Q])>=-Eps) {Inside=true;break;}
                if(Inside) continue;
                if(!AddTriangle(M,Face[A],Face[B],Face[C],Patch,Error)) return false;
                Remaining.RemoveAt(I);Found=true;break;
            }
            if(!Found) {Error=TEXT("OBJ polygon cannot be triangulated. Remove repeated, crossing or collinear edges.");return false;}
        }
        return AddTriangle(M,Face[Remaining[0]],Face[Remaining[1]],Face[Remaining[2]],Patch,Error);
    }
    int32 Patch(FStudioImportedMesh& M,const FString& Name)
    {
        const FString Clean=Name.TrimStartAndEnd().Left(120);const FString Final=Clean.IsEmpty()?TEXT("Surface"):Clean;
        const int32 Existing=M.PatchNames.Find(Final);if(Existing!=INDEX_NONE) return Existing;
        return M.PatchNames.Num()<4096?M.PatchNames.Add(Final):INDEX_NONE;
    }
    bool TextMesh(FStudioImportedMesh& M,const FString& Text,bool OBJ,const FStudioAssetCancellation& Cancel,FString& Error)
    {
        int32 Pos=0,Line=0,CurrentPatch=0,TexCoords=0,Normals=0,STLState=0;TArray<int32> STLFace;
        M.PatchNames.Add(TEXT("Surface"));bool SawSolid=false,SawEnd=false;FString Object;
        while(Pos<Text.Len())
        {
            if(Cancel->load(std::memory_order_relaxed)) return false;
            int32 End=Pos;while(End<Text.Len()&&Text[End]!=TEXT('\n')) ++End;
            if(End-Pos>8192) {Error=TEXT("A mesh source line exceeds 8,192 characters.");return false;}
            FString Row=Text.Mid(Pos,End-Pos);Pos=End+1;++Line;
            int32 Comment;if(Row.FindChar(TEXT('#'),Comment)) Row.LeftInline(Comment);
            Row.TrimStartAndEndInline();if(Row.IsEmpty()) continue;
            TArray<FString> T;Row.ParseIntoArrayWS(T);const FString& K=T[0];
            auto Fail=[&](const FString& Why){Error=FString::Printf(TEXT("Line %d: %s"),Line,*Why);return false;};
            if(OBJ)
            {
                if(K==TEXT("v"))
                {
                    FVector V;if(T.Num()!=4||!Vector(T,1,V)) return Fail(TEXT("Expected three finite vertex coordinates; weighted/colored vertices are not supported."));
                    if(M.Positions.Num()>=StudioMeshImport::MaxVertices) return Fail(TEXT("Vertex limit exceeded."));M.Positions.Add(V);
                }
                else if(K==TEXT("vt")||K==TEXT("vn"))
                {
                    if(T.Num()<2||T.Num()>4||(K==TEXT("vn")&&T.Num()!=4)) return Fail(TEXT("Invalid texture coordinate or normal."));
                    double V;for(int32 I=1;I<T.Num();++I) if(!Number(T[I],V)) return Fail(TEXT("Invalid texture coordinate or normal."));
                    if(K==TEXT("vt")) ++TexCoords;else ++Normals;
                }
                else if(K==TEXT("f"))
                {
                    TArray<int32> Face;
                    for(int32 I=1;I<T.Num();++I)
                    {
                        TArray<FString> Parts;T[I].ParseIntoArray(Parts,TEXT("/"),false);int32 V,Dummy;
                        if(Parts.IsEmpty()||Parts.Num()>3||!Index(Parts[0],M.Positions.Num(),V)) return Fail(TEXT("Face references an invalid vertex."));
                        if(Parts.Num()>=2&&!Parts[1].IsEmpty()&&!Index(Parts[1],TexCoords,Dummy)) return Fail(TEXT("Face references an invalid texture coordinate."));
                        if(Parts.Num()==3&&!Index(Parts[2],Normals,Dummy)) return Fail(TEXT("Face references an invalid normal."));
                        Face.Add(V);
                    }
                    if(!Polygon(M,MoveTemp(Face),CurrentPatch,Error)) return Fail(Error);
                }
                else if(K==TEXT("g")||K==TEXT("o"))
                {
                    const FString Name=Row.Mid(1).TrimStartAndEnd();
                    if(K==TEXT("o")) Object=Name;
                    CurrentPatch=Patch(M,K==TEXT("g")&&!Object.IsEmpty()?Object+TEXT(" / ")+Name:Name);
                    if(CurrentPatch==INDEX_NONE) return Fail(TEXT("Patch limit exceeded."));
                }
                else if(K==TEXT("s")) {}
                else if(K==TEXT("mtllib")||K==TEXT("usemtl")) M.Notes.AddUnique(TEXT("Appearance materials are not imported; physical materials are assigned in the case."));
                else return Fail(TEXT("Unsupported OBJ statement '")+K+TEXT("'. Export polygon faces only."));
            }
            else
            {
                if(K==TEXT("solid")&&STLState==0) {SawSolid=true;SawEnd=false;CurrentPatch=Patch(M,Row.Mid(5));if(CurrentPatch==INDEX_NONE)return Fail(TEXT("Patch limit exceeded."));}
                else if(K==TEXT("facet")&&STLState==0&&SawSolid&&!SawEnd)
                {FVector N;if(T.Num()!=5||T[1]!=TEXT("normal")||!Vector(T,2,N))return Fail(TEXT("Invalid facet normal."));STLState=1;STLFace.Reset();}
                else if(K==TEXT("outer")&&T.Num()==2&&T[1]==TEXT("loop")&&STLState==1) STLState=2;
                else if(K==TEXT("vertex")&&STLState==2&&STLFace.Num()<3)
                {FVector V;if(T.Num()!=4||!Vector(T,1,V))return Fail(TEXT("Invalid STL vertex."));if(M.Positions.Num()>=StudioMeshImport::MaxVertices)return Fail(TEXT("Vertex limit exceeded."));STLFace.Add(M.Positions.Add(V));}
                else if(K==TEXT("endloop")&&T.Num()==1&&STLState==2&&STLFace.Num()==3) STLState=3;
                else if(K==TEXT("endfacet")&&T.Num()==1&&STLState==3)
                {if(!AddTriangle(M,STLFace[0],STLFace[1],STLFace[2],CurrentPatch,Error))return Fail(Error);STLState=0;}
                else if(K==TEXT("endsolid")&&STLState==0&&SawSolid) SawEnd=true;
                else return Fail(TEXT("Invalid ASCII STL structure."));
            }
        }
        if(!OBJ&&(!SawSolid||!SawEnd||STLState!=0)) {Error=TEXT("ASCII STL is incomplete.");return false;}
        return true;
    }
    bool Diagnose(FStudioImportedMesh& M,const FStudioAssetCancellation& Cancel)
    {
        // Exact-position welding is diagnostic only; source vertices remain untouched.
        TMap<FVector,int32> Weld;TArray<int32> Vertices;Vertices.Reserve(M.Positions.Num());
        for(const auto& P:M.Positions)
        {if(Cancel->load(std::memory_order_relaxed))return false;const int32* V=Weld.Find(P);Vertices.Add(V?*V:Weld.Add(P,Weld.Num()));M.Bounds+=P;}
        struct FEdge {int32 Count=0,Direction=0;};TMap<uint64,FEdge> Edges;TSet<FIntVector> Faces;
        for(int32 I=0;I<M.Indices.Num();I+=3)
        {
            if(Cancel->load(std::memory_order_relaxed))return false;
            int32 V[3]={Vertices[M.Indices[I]],Vertices[M.Indices[I+1]],Vertices[M.Indices[I+2]]};
            for(int32 J=0;J<3;++J)
            {const int32 A=V[J],B=V[(J+1)%3];const uint64 Key=(uint64(FMath::Min(A,B))<<32)|uint32(FMath::Max(A,B));auto& E=Edges.FindOrAdd(Key);++E.Count;E.Direction+=A<B?1:-1;}
            if(V[0]>V[1])Swap(V[0],V[1]);if(V[1]>V[2])Swap(V[1],V[2]);if(V[0]>V[1])Swap(V[0],V[1]);
            const FIntVector Key(V[0],V[1],V[2]);if(Faces.Contains(Key))++M.DuplicateFaces;else Faces.Add(Key);
        }
        for(const auto& Item:Edges)
        {const auto& E=Item.Value;if(E.Count==1)++M.BoundaryEdges;else if(E.Count>2)++M.NonmanifoldEdges;else if(E.Direction!=0)++M.InconsistentEdges;}
        // Only patches with faces become assignable persistent patch identities.
        TArray<FString> Used;TMap<int32,int32> Mapping;
        for(int32& P:M.TrianglePatches) {const int32* Found=Mapping.Find(P);if(Found)P=*Found;else{const int32 N=Used.Add(M.PatchNames[P]);Mapping.Add(P,N);P=N;}}
        M.PatchNames=MoveTemp(Used);return true;
    }
}

FStudioMeshImportResult StudioMeshImport::Parse(TArrayView<const uint8> Bytes,const FString& Format,const FStudioAssetCancellation& Cancel)
{
    FStudioMeshImportResult R;R.Format=Format.ToLower();auto M=MakeShared<FStudioImportedMesh,ESPMode::ThreadSafe>();
    auto Cancelled=[&](){R.bCancelled=Cancel->load(std::memory_order_relaxed);if(R.bCancelled)R.Error=TEXT("Import cancelled.");return R.bCancelled;};
    if(Cancelled())return R;
    if(Bytes.IsEmpty()||Bytes.Num()>MaxFileBytes) {R.Error=TEXT("Choose a nonempty geometry file no larger than 64 MiB.");return R;}
    if(R.Format!=TEXT("obj")&&R.Format!=TEXT("stl")) {R.Error=TEXT("Choose an STL or OBJ geometry file.");return R;}
    uint8 Digest[EVP_MAX_MD_SIZE];unsigned int DigestBytes=0;
    if(EVP_Digest(Bytes.GetData(),Bytes.Num(),Digest,&DigestBytes,EVP_sha256(),nullptr)!=1||DigestBytes!=32)
    {R.Error=TEXT("Could not identify the source contents.");return R;}R.SHA256=BytesToHex(Digest,DigestBytes).ToLower();
    const bool Binary=R.Format==TEXT("stl")&&Bytes.Num()>=84&&84LL+50LL*U32(Bytes.GetData()+80)==Bytes.Num();
    if(Binary)
    {
        const uint32 Count=U32(Bytes.GetData()+80);
        if(Count>MaxTriangles) {R.Error=TEXT("Mesh exceeds the 500,000-triangle import limit.");return R;}
        M->PatchNames.Add(TEXT("Surface"));M->Positions.Reserve(Count*3);M->Indices.Reserve(Count*3);
        for(uint32 I=0;I<Count;++I)
        {
            if(Cancelled())return R;const uint8* P=Bytes.GetData()+84+50*I;const int32 Base=M->Positions.Num();
            for(int32 J=0;J<3;++J)
            {const uint8* V=P+12+12*J;const FVector Point(Float(V),Float(V+4),Float(V+8));if(Point.ContainsNaN()||Point.GetAbsMax()>1.e8){R.Error=TEXT("Binary STL contains a nonfinite or out-of-range vertex.");return R;}M->Positions.Add(Point);}
            if(!AddTriangle(*M,Base,Base+1,Base+2,0,R.Error))return R;
        }
    }
    else
    {
        for(uint8 B:Bytes) if(B==0) {R.Error=TEXT("Invalid text mesh or truncated binary STL.");return R;}
        int32 Start=Bytes.Num()>=3&&Bytes[0]==0xef&&Bytes[1]==0xbb&&Bytes[2]==0xbf?3:0;
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Start),Bytes.Num()-Start);
        if(!TextMesh(*M,FString(Converted.Length(),Converted.Get()),R.Format==TEXT("obj"),Cancel,R.Error)) {Cancelled();return R;}
    }
    if(M->Indices.IsEmpty()) {R.Error=TEXT("The file contains no polygon triangles.");return R;}
    if(!Diagnose(*M,Cancel)||Cancelled()) {Cancelled();return R;}
    R.Mesh=M;return R;
}
FStudioMeshImportResult StudioMeshImport::Read(const FString& Path,const FStudioAssetCancellation& Cancel)
{
    FStudioMeshImportResult R;R.Path=Path;R.Format=FPaths::GetExtension(Path).ToLower();FStudioFileAccess Access(Path);
    const FDateTime Time=IFileManager::Get().GetTimeStamp(*Path);TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!File) {R.Error=TEXT("Cannot read the geometry file. Choose an accessible STL or OBJ.");return R;}
    const int64 Size=File->TotalSize();if(Size<=0||Size>MaxFileBytes){R.Error=TEXT("Choose a nonempty geometry file no larger than 64 MiB.");return R;}
    TArray<uint8> Bytes;Bytes.SetNumUninitialized(int32(Size));
    for(int64 Offset=0;Offset<Size;)
    {
        if(Cancel->load(std::memory_order_relaxed)){R.bCancelled=true;R.Error=TEXT("Import cancelled.");return R;}
        const int64 Count=FMath::Min<int64>(64*1024,Size-Offset);File->Serialize(Bytes.GetData()+Offset,Count);Offset+=Count;
        if(File->IsError()){R.Error=TEXT("Could not read the complete geometry file.");return R;}
    }
    R=Parse(Bytes,R.Format,Cancel);R.Path=FPaths::ConvertRelativePathToFull(Path);
    if(Time!=IFileManager::Get().GetTimeStamp(*Path)||Size!=IFileManager::Get().FileSize(*Path)) {R.Mesh.Reset();R.Error=TEXT("The source changed during import. Wait for writing to finish and choose it again.");}
    return R;
}
FQuat StudioMeshImport::AxisRotation(int32 UpAxis,int32 ForwardAxis)
{
    if(UpAxis<0||UpAxis>2||ForwardAxis<0||ForwardAxis>2||UpAxis==ForwardAxis)return FQuat::Identity;
    FVector Up=FVector::ZeroVector,Forward=FVector::ZeroVector;Up[UpAxis]=1;Forward[ForwardAxis]=1;
    return FRotationMatrix::MakeFromXZ(Forward,Up).ToQuat().Inverse();
}
FVector StudioMeshImport::TransformPosition(const FVector& P,const FStudioGeometryAsset& A)
{return A.Rotation.RotateVector(P*A.MetersPerSourceUnit*A.Scale)+A.Translation;}
FBox StudioMeshImport::TransformedBounds(const FStudioImportedMesh& M,const FStudioGeometryAsset& A)
{
    FBox Result(ForceInit);if(!M.Bounds.IsValid)return Result;
    for(int32 I=0;I<8;++I)Result+=TransformPosition(FVector(I&1?M.Bounds.Max.X:M.Bounds.Min.X,I&2?M.Bounds.Max.Y:M.Bounds.Min.Y,I&4?M.Bounds.Max.Z:M.Bounds.Min.Z),A);
    return Result;
}
bool StudioMeshImport::MakeAsset(const FStudioMeshImportResult& R,const FStudioMeshImportOptions& O,FStudioGeometryAsset& Out,FString& Error)
{
    if(!R.IsValid()) {Error=TEXT("Load and preview a valid mesh first.");return false;}
    if(O.Name.TrimStartAndEnd().IsEmpty()||O.Name.Len()>120){Error=TEXT("Enter a geometry name of 1–120 characters.");return false;}
    if(!FMath::IsFinite(O.MetersPerUnit)||O.MetersPerUnit<=0||O.MetersPerUnit>1.e6){Error=TEXT("Choose the source units before importing.");return false;}
    if(O.UpAxis<0||O.UpAxis>2||O.ForwardAxis<0||O.ForwardAxis>2||O.UpAxis==O.ForwardAxis){Error=TEXT("Up and forward must be different axes.");return false;}
    FStudioGeometryAsset A;A.Name=O.Name.TrimStartAndEnd();A.SourcePath=R.Path;A.SourceSHA256=R.SHA256;A.Format=R.Format;
    A.MetersPerSourceUnit=O.MetersPerUnit;A.Rotation=AxisRotation(O.UpAxis,O.ForwardAxis);
    for(const auto& Name:R.Mesh->PatchNames){FStudioSurfacePatch P;P.Name=Name;A.Patches.Add(P);}
    const auto Box=TransformedBounds(*R.Mesh,A);
    if(!Box.IsValid||Box.Min.GetAbsMax()>1.e8||Box.Max.GetAbsMax()>1.e8){Error=TEXT("Converted geometry exceeds the supported coordinate range. Check the source units.");return false;}
    Out=MoveTemp(A);Error.Empty();return true;
}
