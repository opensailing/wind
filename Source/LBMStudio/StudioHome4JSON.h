#pragma once
#include "CoreMinimal.h"
#include "Serialization/JsonReader.h"

/** Strict checks before allocating a JSON object tree. Limits belong to the caller. */
namespace StudioHome4JSON
{
    inline bool Unicode(const FString& Text)
    {
        for (int32 I = 0; I < Text.Len(); ++I)
        {
            const uint32 C = uint32(Text[I]);
            if (C == 0) return false;
            if constexpr (sizeof(TCHAR) == 2)
            {
                if (C >= 0xd800 && C <= 0xdbff)
                { if (++I >= Text.Len() || uint32(Text[I]) < 0xdc00 || uint32(Text[I]) > 0xdfff) return false; }
                else if (C >= 0xdc00 && C <= 0xdfff) return false;
            }
            else if (C > 0x10ffff || (C >= 0xd800 && C <= 0xdfff)) return false;
        }
        return true;
    }
    inline bool Preflight(const FString& JSON, int32 MaxDepth = 32)
    {
        if (!Unicode(JSON)) return false;
        // Check UTF-16 escapes before the engine can replace an invalid surrogate.
        bool InString = false;
        auto EscapeCode = [&](int32 Start, uint32& Out)
        {
            if (Start + 4 > JSON.Len()) return false;
            Out = 0;
            for (int32 J = 0; J < 4; ++J)
            {
                const TCHAR C = JSON[Start + J];
                const int32 Digit = C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : C >= 'A' && C <= 'F' ? C - 'A' + 10 : -1;
                if (Digit < 0) return false;
                Out = (Out << 4) | uint32(Digit);
            }
            return true;
        };
        for (int32 I = 0; I < JSON.Len(); ++I)
        {
            if (JSON[I] == '"') { InString = !InString; continue; }
            if (!InString || JSON[I] != '\\') continue;
            if (++I >= JSON.Len()) return false;
            if (JSON[I] != 'u') continue;
            uint32 Code = 0;
            if (!EscapeCode(I + 1, Code)) return false;
            I += 4;
            if (Code >= 0xdc00 && Code <= 0xdfff) return false;
            if (Code >= 0xd800 && Code <= 0xdbff)
            {
                uint32 Low = 0;
                if (I + 2 >= JSON.Len() || JSON[I + 1] != '\\' || JSON[I + 2] != 'u' || !EscapeCode(I + 3, Low) || Low < 0xdc00 || Low > 0xdfff) return false;
                I += 6;
            }
        }
        const auto Reader = TJsonReaderFactory<>::Create(JSON);
        EJsonNotation Token; TArray<bool> Containers; TArray<TSet<FString>> Keys;
        bool bRoot = false;
        while (Reader->ReadNext(Token))
        {
            if (Token == EJsonNotation::Error || (Token == EJsonNotation::Number && !FMath::IsFinite(Reader->GetValueAsNumber()))) return false;
            if (Token == EJsonNotation::String && !Unicode(Reader->GetValueAsString())) return false;
            if (Containers.IsEmpty() && Token != EJsonNotation::ObjectEnd && Token != EJsonNotation::ArrayEnd)
            { if (bRoot) return false; bRoot = true; }
            if (Token != EJsonNotation::ObjectEnd && Token != EJsonNotation::ArrayEnd && !Containers.IsEmpty() && Containers.Last())
            {
                const FString Key = Reader->GetIdentifier();
                if (!Unicode(Key)) return false;
                if (Keys.Last().Contains(Key)) return false;
                Keys.Last().Add(Key);
            }
            if (Token == EJsonNotation::ObjectStart || Token == EJsonNotation::ArrayStart)
            {
                if (Containers.Num() >= MaxDepth) return false;
                const bool Object = Token == EJsonNotation::ObjectStart;
                Containers.Add(Object); if (Object) Keys.Add(TSet<FString>());
            }
            else if (Token == EJsonNotation::ObjectEnd || Token == EJsonNotation::ArrayEnd)
            {
                if (Containers.IsEmpty() || Containers.Last() != (Token == EJsonNotation::ObjectEnd)) return false;
                if (Containers.Last()) Keys.Pop(EAllowShrinking::No);
                Containers.Pop(EAllowShrinking::No);
            }
        }
        return bRoot && Containers.IsEmpty() && Reader->GetErrorMessage().IsEmpty();
    }
    inline bool UTF8(const uint8* Bytes, int32 Size)
    {
        if (!Bytes || Size <= 0) return false;
        for (int32 I = 0; I < Size;)
        {
            const uint8 C = Bytes[I++]; if (C < 0x80) { if (!C) return false; continue; }
            int32 N = 0; uint32 Code = 0, Minimum = 0;
            if (C >= 0xc2 && C <= 0xdf) { N = 1; Code = C & 31; Minimum = 0x80; }
            else if (C >= 0xe0 && C <= 0xef) { N = 2; Code = C & 15; Minimum = 0x800; }
            else if (C >= 0xf0 && C <= 0xf4) { N = 3; Code = C & 7; Minimum = 0x10000; }
            else return false;
            if (Size - I < N) return false;
            for (int32 J = 0; J < N; ++J)
            { const uint8 Next = Bytes[I++]; if ((Next & 0xc0) != 0x80) return false; Code = (Code << 6) | (Next & 63); }
            if (Code < Minimum || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)) return false;
        }
        return true;
    }
}
