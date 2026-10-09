#include "StudioRecording.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

TArray<FStudioRecordingEntry> StudioRecordings::Installed()
{
    TArray<FStudioRecordingEntry> Result;
    const FString Root=FPaths::ProjectContentDir()/TEXT("Samples");
    const FString Path=Root/TEXT("Registry/recordings.json");
    const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<0||Size>65536) return Result;
    FString Text; TSharedPtr<FJsonObject> O; const TArray<TSharedPtr<FJsonValue>>* Entries=nullptr;
    if(!FFileHelper::LoadFileToString(Text,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||
        !O||!O->TryGetArrayField(TEXT("recordings"),Entries)||Entries->Num()>128) return Result;
    TSet<FString> Ids;
    for(const auto& Value:*Entries)
    {
        const TSharedPtr<FJsonObject>* Item=nullptr; FStudioRecordingEntry Entry; FString Relative;
        if(!Value->TryGetObject(Item)||!(*Item)->TryGetStringField(TEXT("id"),Entry.Id)||Entry.Id.IsEmpty()||Entry.Id.Len()>256||
            !(*Item)->TryGetStringField(TEXT("title"),Entry.Title)||Entry.Title.IsEmpty()||Entry.Title.Len()>256||
            !(*Item)->TryGetStringField(TEXT("path"),Relative)||!FPaths::IsRelative(Relative)||Relative.Contains(TEXT(".."))||Relative.Contains(TEXT("\\"))||
            !Relative.EndsWith(TEXT("/flow.bin"))||Ids.Contains(Entry.Id)) return {};
        Entry.Path=Root/Relative; Ids.Add(Entry.Id); Result.Add(MoveTemp(Entry));
    }
    return Result;
}
FString StudioRecordings::PathForId(const FString& Id)
{
    for(const auto& Entry:Installed()) if(Entry.Id==Id) return Entry.Path;
    return {};
}
