// Copyright FlowViz contributors. All Rights Reserved.

#include "Capture/FlowVizAnnotate.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizAnnotateTest
{
	constexpr int32 TestWidth = 640;
	constexpr int32 TestHeight = 360;

	/** A sentinel no annotation colour uses, so untouched pixels are provable. */
	const FColor Sentinel(1, 2, 3, 255);

	TArray<FColor> MakeImage()
	{
		TArray<FColor> Pixels;
		Pixels.Init(Sentinel, TestWidth * TestHeight);
		return Pixels;
	}

	FFlowVizCaptureAnnotation MakeAnnotation()
	{
		FFlowVizCaptureAnnotation Annotation;
		Annotation.CaseName = TEXT("MockCylinderWake");
		Annotation.FieldName = TEXT("U");
		Annotation.Time = 0.24;
		Annotation.TimeUnit = TEXT("s");
		Annotation.RangeMin = -12.5f;
		Annotation.RangeMax = 33.25f;
		Annotation.ColorMap = ECFDVizColorMap::Viridis;
		return Annotation;
	}
}

/**
 * Screenshot annotation (#83 / Milestone F, DoD 15): the footer carries the
 * identity and the legend, and touches ONLY the footer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizAnnotateTest,
	"FlowViz.Capture.Annotate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizAnnotateTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizAnnotateTest;

	/* == The footer burns, and ONLY the footer ============================== */
	{
		TArray<FColor> Pixels = MakeImage();
		TestTrue(TEXT("the footer burns"),
			FlowVizAnnotate::BurnFooter(
				Pixels, TestWidth, TestHeight, MakeAnnotation()));

		// Every pixel ABOVE the footer is byte-identical: the annotation must
		// never paint over data pixels -- a caption across the wake would be
		// the one thing worse than no caption.
		int32 TouchedAbove = 0;
		const int32 FooterTop = TestHeight - FlowVizAnnotate::FooterHeight;
		for (int32 Y = 0; Y < FooterTop; ++Y)
		{
			for (int32 X = 0; X < TestWidth; ++X)
			{
				if (Pixels[Y * TestWidth + X] != Sentinel)
				{
					++TouchedAbove;
				}
			}
		}
		TestEqual(TEXT("no pixel above the footer is touched"), TouchedAbove, 0);

		// The footer itself is fully painted: no sentinel survives, so the
		// strip is opaque rather than the render bleeding through the caption.
		int32 SentinelInFooter = 0;
		for (int32 Y = FooterTop; Y < TestHeight; ++Y)
		{
			for (int32 X = 0; X < TestWidth; ++X)
			{
				if (Pixels[Y * TestWidth + X] == Sentinel)
				{
					++SentinelInFooter;
				}
			}
		}
		TestEqual(TEXT("the footer strip is fully painted"), SentinelInFooter, 0);
	}

	/* == The legend samples the REAL colormap ================================ */
	{
		TArray<FColor> Viridis = MakeImage();
		TArray<FColor> Inferno = MakeImage();
		FFlowVizCaptureAnnotation Annotation = MakeAnnotation();
		FlowVizAnnotate::BurnFooter(Viridis, TestWidth, TestHeight, Annotation);
		Annotation.ColorMap = ECFDVizColorMap::Inferno;
		FlowVizAnnotate::BurnFooter(Inferno, TestWidth, TestHeight, Annotation);

		int32 Different = 0;
		for (int32 Index = 0; Index < Viridis.Num(); ++Index)
		{
			if (Viridis[Index] != Inferno[Index])
			{
				++Different;
			}
		}
		TestTrue(TEXT("changing the colormap changes the burned legend -- the strip samples "
					  "the real table, not a baked gradient"),
			Different > 100);

		/* -- Reversal flips the strip's ends. ------------------------------- */
		TArray<FColor> Reversed = MakeImage();
		Annotation.ColorMap = ECFDVizColorMap::Viridis;
		Annotation.bReversed = true;
		FlowVizAnnotate::BurnFooter(Reversed, TestWidth, TestHeight, Annotation);
		int32 ReversalChanged = 0;
		for (int32 Index = 0; Index < Viridis.Num(); ++Index)
		{
			if (Viridis[Index] != Reversed[Index])
			{
				++ReversalChanged;
			}
		}
		TestTrue(TEXT("reversing the map flips the legend"), ReversalChanged > 100);
	}

	/* == Text really renders ================================================= */
	{
		TArray<FColor> Pixels = MakeImage();
		const FColor Ink(255, 0, 0, 255);
		FlowVizAnnotate::DrawText(Pixels, TestWidth, TestHeight, 10, 10, TEXT("A"), Ink, 1);

		int32 InkCount = 0;
		for (const FColor& Pixel : Pixels)
		{
			if (Pixel == Ink)
			{
				++InkCount;
			}
		}
		// 'A' = {7E,11,11,11,7E} inks exactly 18 bits; the exact count pins
		// the glyph table -- an off-by-one in the bit unpacking moves it.
		TestEqual(TEXT("the glyph 'A' inks its exact pixel count"), InkCount, 18);

		// Clipping: text at a negative origin must not wrap or crash.
		FlowVizAnnotate::DrawText(Pixels, TestWidth, TestHeight, -3, -3, TEXT("XX"), Ink, 1);
		TestTrue(TEXT("clipped text neither crashed nor wrapped to the far edge"),
			Pixels[(TestHeight - 1) * TestWidth + (TestWidth - 1)] == Sentinel);
	}

	/* == Refusals ============================================================ */
	{
		TArray<FColor> Tiny;
		Tiny.Init(Sentinel, 64 * 64);
		TestFalse(TEXT("an image too small for the footer is refused whole"),
			FlowVizAnnotate::BurnFooter(Tiny, 64, 64, MakeAnnotation()));
		int32 Touched = 0;
		for (const FColor& Pixel : Tiny)
		{
			if (Pixel != Sentinel)
			{
				++Touched;
			}
		}
		TestEqual(TEXT("and untouched -- half an annotation is worse than none"), Touched, 0);

		TArray<FColor> Mismatched;
		Mismatched.Init(Sentinel, 100);
		TestFalse(TEXT("a buffer that disagrees with the stated size is refused"),
			FlowVizAnnotate::BurnFooter(
				Mismatched, TestWidth, TestHeight, MakeAnnotation()));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
