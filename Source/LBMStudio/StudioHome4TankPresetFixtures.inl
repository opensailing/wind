#pragma once
namespace StudioHome4TankPresetFixtures
{
inline FStudioHome4Spec Spec(bool Preset)
{
    FStudioHome4Spec S;S.RecipeId=TEXT("th01-hull");S.Reference.LengthCells=4;S.Reference.SpeedCellsPerStep=.02;S.Fluids.Gravity=.001;S.Fluids.NuHeavy=.01;
    S.Authoring.BodyId=TEXT("original-body");S.Authoring.Primitive=TEXT("box");S.Authoring.PrimitiveSizeCells=FVector(2);S.Geometry.InitialPositionCells=FVector(4);S.Geometry.CenterOfGravity=FVector(4);
    S.Lattice.Extents=Preset?FIntVector(20,12,10):FIntVector(16);S.Lattice.PadUp=Preset?2:1;S.Lattice.PadDown=3;S.Lattice.PadSide=1;S.Lattice.Depth=1;S.Lattice.Air=1;
    S.Authoring.WaterlineCells=4;S.Authoring.ZoneUnits=TEXT("root-cells");S.Zones.ZoneStrength=.3;
    if(Preset){S.Zones.Sponge=4;S.Zones.PeriodicY=false;FStudioHome4AuthoredZone Z;Z.Id=TEXT("original-downstream");Z.Minimum=FVector(16,0,0);Z.Maximum=FVector(20,12,10);Z.Strength=.3;Z.LevelExponent=.5;S.Authoring.Zones.Add(Z);}
    return S;
}
inline FString JSON(const TSharedRef<FJsonObject>& O){FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
inline FString Definition(const FString& Name,const FStudioHome4Spec& S)
{
    auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("schema"),TEXT("LBMStudio.Home4TankZonePreset"));O->SetNumberField(TEXT("version"),1);O->SetStringField(TEXT("name"),Name);
    O->SetStringField(TEXT("source_id"),TEXT("Artificial supplied tank definition; never a published TH01 numeric preset"));O->SetObjectField(TEXT("run_spec"),StudioHome4Config::ToJSON(S));return JSON(O);
}
}
