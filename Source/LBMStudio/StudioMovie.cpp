#include "StudioMovie.h"
#include "StudioSnapshot.h"

bool StudioMovie::Supported(){return PLATFORM_MAC;}
bool StudioMovie::Validate(const FStudioMovieOptions& O,FIntPoint Size,FString& Error)
{
    Error.Empty();if(!O.bEnabled)return true;
    if(!Supported()){Error=TEXT("MP4 encoding is available on macOS. Export PNG images on this platform.");return false;}
    if(O.FrameRate<1||O.FrameRate>60){Error=TEXT("Choose a whole-number movie rate from 1 to 60 frames per second.");return false;}
    if(!StudioSnapshot::ValidSize(Size)||(Size.X%2)||(Size.Y%2))
    {Error=TEXT("MP4 requires even image dimensions from 64 to 4096 pixels. Choose an even width and a compatible frame aspect.");return false;}
    return true;
}
#if !PLATFORM_MAC
TUniquePtr<IStudioMovieEncoder> IStudioMovieEncoder::Create(){return {};}
#endif
