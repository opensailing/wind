#include "../StudioMovie.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Mac/MacSystemIncludes.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

// The replacement pixel-buffer receiver is Swift-only. This Objective-C API
// remains available on our supported macOS versions, including the current SDK.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
namespace
{
class FMacMovieEncoder final : public IStudioMovieEncoder
{
    AVAssetWriter* Writer=nil;
    AVAssetWriterInput* Input=nil;
    AVAssetWriterInputPixelBufferAdaptor* Adaptor=nil;
    FIntPoint Size=FIntPoint::ZeroValue;
    int32 Rate=0,Count=0;
    bool bFinished=false;
    FString Failure(const TCHAR* Action) const
    {
        NSString* Detail=Writer.error.localizedDescription;
        return FString(Action)+(Detail?TEXT(" ")+FString(UTF8_TO_TCHAR(Detail.UTF8String)):FString());
    }
public:
    ~FMacMovieEncoder() override
    {
        @autoreleasepool
        {
            if(Writer&&!bFinished)[Writer cancelWriting];
            [Adaptor release];[Input release];[Writer release];
        }
    }
    bool Begin(const FString& Path,FIntPoint InSize,int32 FrameRate,const FString& Description,FString& Error) override
    {
        @autoreleasepool
        {
            if(Writer){Error=TEXT("This movie encoder has already started.");return false;}
            if(!StudioMovie::Validate({true,FrameRate},InSize,Error))return false;
            Size=InSize;Rate=FrameRate;
            NSURL* URL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Path)]];
            NSError* NativeError=nil;
            Writer=[[AVAssetWriter alloc] initWithURL:URL fileType:AVFileTypeMPEG4 error:&NativeError];
            if(!Writer){Error=TEXT("Could not create the MP4. Check destination access and free space. ")+FString(UTF8_TO_TCHAR(NativeError.localizedDescription.UTF8String));return false;}
            const int32 BitRate=int32(FMath::Clamp<int64>(int64(Size.X)*Size.Y*Rate/2,1000000,80000000));
            NSDictionary* Settings=@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(Size.X),AVVideoHeightKey:@(Size.Y),
                AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(BitRate),AVVideoExpectedSourceFrameRateKey:@(Rate),
                    AVVideoMaxKeyFrameIntervalKey:@(Rate),AVVideoAllowFrameReorderingKey:@NO,AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel},
                AVVideoColorPropertiesKey:@{AVVideoColorPrimariesKey:AVVideoColorPrimaries_ITU_R_709_2,
                    AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2,AVVideoYCbCrMatrixKey:AVVideoYCbCrMatrix_ITU_R_709_2}};
            if(![Writer canApplyOutputSettings:Settings forMediaType:AVMediaTypeVideo])
            {Error=TEXT("The native encoder cannot use this image size and rate. Reduce them or export PNGs.");return false;}
            Input=[[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:Settings];
            Input.expectsMediaDataInRealTime=NO;
            if(![Writer canAddInput:Input]){Error=TEXT("Could not attach the native H.264 encoder.");return false;}
            [Writer addInput:Input];
            Adaptor=[[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:Input sourcePixelBufferAttributes:
                @{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                  (id)kCVPixelBufferWidthKey:@(Size.X),(id)kCVPixelBufferHeightKey:@(Size.Y),
                  (id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
            AVMutableMetadataItem* Note=[AVMutableMetadataItem metadataItem];
            Note.keySpace=AVMetadataKeySpaceiTunes;Note.key=AVMetadataiTunesMetadataKeyUserComment;
            Note.dataType=(NSString*)kCMMetadataBaseDataType_UTF8;
            Note.value=[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Description)];Writer.metadata=@[Note];
            if(![Writer startWriting]){Error=Failure(TEXT("Could not start MP4 encoding."));return false;}
            [Writer startSessionAtSourceTime:kCMTimeZero];Error.Empty();return true;
        }
    }
    bool Append(TConstArrayView<FColor> Pixels,TFunctionRef<bool()> Cancelled,FString& Error) override
    {
        @autoreleasepool
        {
            if(!Writer||bFinished||Pixels.Num()!=int64(Size.X)*Size.Y)
            {Error=TEXT("The movie frame does not match the frozen image size.");return false;}
            const double Deadline=FPlatformTime::Seconds()+30.;
            CVPixelBufferRef Buffer=nullptr;
            ON_SCOPE_EXIT {if(Buffer)CVPixelBufferRelease(Buffer);};
            for(;;)
            {
                if(Cancelled()){Error=TEXT("Movie encoding cancelled.");return false;}
                if(Writer.status!=AVAssetWriterStatusWriting){Error=Failure(TEXT("MP4 encoding failed."));return false;}
                if(Input.readyForMoreMediaData)
                {
                    const auto Status=Adaptor.pixelBufferPool?CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,Adaptor.pixelBufferPool,
                        (CFDictionaryRef)@{(id)kCVPixelBufferPoolAllocationThresholdKey:@3},&Buffer):kCVReturnInvalidPoolAttributes;
                    if(Status==kCVReturnSuccess)break;
                    if(Status!=kCVReturnWouldExceedAllocationThreshold){Error=TEXT("Could not allocate a native movie frame. Reduce image dimensions.");return false;}
                }
                if(FPlatformTime::Seconds()>Deadline){Error=TEXT("The native movie encoder stopped accepting frames. Retry with a smaller image or lower rate.");return false;}
                FPlatformProcess::SleepNoStats(.002f);
            }
            if(CVPixelBufferLockBaseAddress(Buffer,0)!=kCVReturnSuccess){Error=TEXT("Could not access the native movie frame.");return false;}
            const size_t RowBytes=CVPixelBufferGetBytesPerRow(Buffer);
            auto* Base=static_cast<uint8*>(CVPixelBufferGetBaseAddress(Buffer));
            for(int32 Y=0;Y<Size.Y;++Y)
            {
                auto* Row=Base+Y*RowBytes;
                for(int32 X=0;X<Size.X;++X)
                {const auto& P=Pixels[Y*Size.X+X];Row[4*X]=P.B;Row[4*X+1]=P.G;Row[4*X+2]=P.R;Row[4*X+3]=255;}
            }
            CVPixelBufferUnlockBaseAddress(Buffer,0);
            if(Cancelled()){Error=TEXT("Movie encoding cancelled.");return false;}
            if(![Adaptor appendPixelBuffer:Buffer withPresentationTime:CMTimeMake(Count,Rate)])
            {Error=Failure(TEXT("Could not append the original frame to MP4."));return false;}
            ++Count;Error.Empty();return true;
        }
    }
    bool Finish(TFunctionRef<bool()> Cancelled,FString& Error) override
    {
        @autoreleasepool
        {
            if(!Writer||bFinished||Count<1){Error=TEXT("No complete movie frames are available.");return false;}
            if(Cancelled()){Error=TEXT("Movie encoding cancelled.");return false;}
            if(Writer.status!=AVAssetWriterStatusWriting){Error=Failure(TEXT("MP4 encoding failed."));return false;}
            [Writer endSessionAtSourceTime:CMTimeMake(Count,Rate)];[Input markAsFinished];
            const auto Done=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
            [Writer finishWritingWithCompletionHandler:^{Done->store(true);}];
            const double Deadline=FPlatformTime::Seconds()+30.;
            while(!Done->load())
            {
                if(Cancelled()||FPlatformTime::Seconds()>Deadline)
                {
                    [Writer cancelWriting];
                    Error=Cancelled()?TEXT("Movie encoding cancelled."):TEXT("Finalizing the MP4 timed out. Retry a smaller export.");return false;
                }
                FPlatformProcess::SleepNoStats(.002f);
            }
            if(Writer.status!=AVAssetWriterStatusCompleted){Error=Failure(TEXT("Could not finish the MP4. Check free space."));return false;}
            bFinished=true;Error.Empty();return true;
        }
    }
};
}
TUniquePtr<IStudioMovieEncoder> IStudioMovieEncoder::Create(){return MakeUnique<FMacMovieEncoder>();}
PRAGMA_ENABLE_DEPRECATION_WARNINGS
