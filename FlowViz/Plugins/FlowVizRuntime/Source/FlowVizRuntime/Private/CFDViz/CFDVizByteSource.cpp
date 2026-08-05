// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizByteSource.h"

#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FCFDVizFileByteSource::~FCFDVizFileByteSource()
{
	Close();
}

bool FCFDVizFileByteSource::Open(const FString& InPath)
{
	Close();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	IFileHandle* Opened = PlatformFile.OpenRead(*InPath, /*bAllowWrite=*/false);
	if (Opened == nullptr)
	{
		return false;
	}

	Handle = Opened;
	FileSize = Opened->Size();
	DisplayPath = InPath;

	// A negative size means the handle is unusable. Leaving it open with a
	// nonsense size would make Contains() accept spans that cannot be read, and
	// every later failure would surface as a mysterious short read instead of
	// "could not open".
	if (FileSize < 0)
	{
		Close();
		return false;
	}
	return true;
}

void FCFDVizFileByteSource::Close()
{
	if (Handle != nullptr)
	{
		delete Handle;
		Handle = nullptr;
	}
	FileSize = 0;
	DisplayPath.Reset();
}

bool FCFDVizFileByteSource::Read(int64 Offset, int64 Count, void* OutBuffer) const
{
	if (Handle == nullptr || !Contains(Offset, Count))
	{
		return false;
	}
	if (Count == 0)
	{
		return true;
	}
	if (OutBuffer == nullptr)
	{
		return false;
	}

	// Seek and Read are separate calls, so this handle cannot be shared across
	// threads - each thread gets its own source, as the interface documents.
	if (!Handle->Seek(Offset))
	{
		return false;
	}
	// IFileHandle::Read is all-or-nothing; a partial read reports false rather
	// than a short count, which is what the interface contract requires. A
	// caller that parsed the uninitialised tail of a partial read would be
	// parsing stack garbage.
	return Handle->Read(static_cast<uint8*>(OutBuffer), Count);
}
