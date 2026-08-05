// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizVolumeReader.h"
#include "CFDViz/CFDVizByteSource.h"
#include "CFDViz/CFDVizCrc32C.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CVF reader conformance (format section 4).
 *
 * BOTH FIXTURES BELOW ARE BYTES THE PYTHON REFERENCE WRITER ACTUALLY PRODUCED,
 * captured from `cfdviz.cvf.write_cvf(...)` and pasted verbatim. That is what
 * makes this a cross-implementation check rather than a self-consistency check:
 * every offset, every CRC, the zlib stream and the float bit patterns come from
 * the other implementation. A round-trip through this reader alone would pass
 * just as happily if reader and writer shared the same wrong idea of the layout,
 * which is the entire reason two implementations exist.
 *
 * THE GEOMETRY IS DELIBERATELY ASYMMETRIC: dimensions (5, 3, 6) with brick size
 * (3, 2, 4). No two axes are the same length, no axis is a multiple of its brick
 * edge, and the brick grid is 2x2x2 so EVERY axis has both an interior brick and
 * a clipped edge brick (3->3+2, 3->2+1, 6->4+2). A cube fixture like (8,8,8)
 * with brick size 8 cannot fail an axis-transposition test, a partial-brick test
 * or an off-by-one, because every wrong answer coincides with the right one.
 */

namespace CvfReaderTest
{
	/* ---------------------------------------------------------------------- */
	/* NAMED NAMESPACE ON PURPOSE. FlowVizRuntime is a unity build: every .cpp  */
	/* in the module is concatenated into one translation unit, so an anonymous */
	/* namespace here would NOT be private to this file and these constants     */
	/* would collide with a sibling test's. See Docs/BUILD.md.                  */
	/* ---------------------------------------------------------------------- */

	/** On-disk offsets this test pokes at, from format section 4.1. */
	constexpr int32 HdrOffsetMagic = 0;
	constexpr int32 HdrOffsetHeaderBytes = 8;
	constexpr int32 HdrOffsetMajorVersion = 12;
	constexpr int32 HdrOffsetEndianMarker = 16;
	constexpr int32 HdrOffsetFlags = 20;
	constexpr int32 HdrOffsetFrameIndex = 24;
	constexpr int32 HdrOffsetFieldNumericId = 28;
	constexpr int32 HdrOffsetDimensions = 40;
	constexpr int32 HdrOffsetBrickSize = 52;
	constexpr int32 HdrOffsetComponentCount = 58;
	constexpr int32 HdrOffsetDataType = 59;
	constexpr int32 HdrOffsetAssociation = 60;
	constexpr int32 HdrOffsetCodec = 61;
	constexpr int32 HdrOffsetReservedShort = 62;
	constexpr int32 HdrOffsetBrickCount = 64;
	constexpr int32 HdrOffsetDirectoryOffset = 72;
	constexpr int32 HdrOffsetPayloadOffset = 80;
	constexpr int32 HdrOffsetBackground = 88;
	constexpr int32 HdrOffsetCrc = 104;
	constexpr int32 HdrOffsetReservedTail = 108;
	constexpr int32 HdrBytes = 128;

	/** On-disk offsets within one 80-byte directory entry, from section 4.3. */
	constexpr int32 CvfTestEntryOffsetBrickIndex = 0;
	constexpr int32 CvfTestEntryOffsetValidSize = 12;
	constexpr int32 CvfTestEntryOffsetEntryFlags = 18;
	constexpr int32 CvfTestEntryOffsetPayload = 20;
	constexpr int32 CvfTestEntryOffsetCompressed = 28;
	constexpr int32 CvfTestEntryOffsetUncompressed = 32;
	constexpr int32 CvfTestEntryOffsetComponentMin = 36;
	constexpr int32 CvfTestEntryOffsetComponentMax = 52;
	constexpr int32 CvfTestEntryOffsetPayloadCrc = 68;
	constexpr int32 CvfTestEntryOffsetReserved = 72;
	constexpr int32 CvfTestEntryBytes = 80;

	/** Where the golden fixture's directory and payloads start. */
	constexpr int32 GoldenDirectoryOffset = 128;
	constexpr int32 GoldenPayloadOffset = 768;

	/**
	 * dimensions (5, 3, 6) cell-associated, brick size (3, 2, 4), uint8, TWO
	 * components, codec none, fully dense (all 8 bricks present).
	 *
	 * Component 0 holds the flat index x + 5y + 15z and component 1 holds
	 * 89 - that index, so every voxel in the volume is unique AND the two
	 * components differ everywhere except at the middle. A reader that
	 * transposed X and Z, dropped the component interleave, or reconstructed an
	 * edge brick as a full 3x2x4 cube lands on a different byte than the one
	 * asserted below - which a fixture of all zeros, or a symmetric one, could
	 * not detect.
	 *
	 * codec none is chosen so payload bytes are the stored voxels verbatim:
	 * every expected value in this test is readable directly out of the array
	 * below, and a decode bug cannot hide behind a compressed blob.
	 *
	 * From `write_cvf(values=v, brick_size=(3,2,4), frame_index=7,
	 *                 field_numeric_id=42, simulation_time=0.5,
	 *                 association='cell', dtype='uint8', codec=0)`.
	 */
	const uint8 GoldenCvfBytes[] = {
		// ---- header, offset 0 (format section 4.1) ----------------------
		0x43, 0x46, 0x44, 0x56, 0x4F, 0x4C, 0x31, 0x00,                                                  // 0   magic "CFDVOL1\0"
		0x80, 0x00, 0x00, 0x00,                                                                          // 8   headerBytes = 128
		0x01, 0x00, 0x00, 0x00,                                                                          // 12  majorVersion = 1, minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                                                                          // 16  endianMarker = 0x01020304
		0x00, 0x00, 0x00, 0x00,                                                                          // 20  flags = 0
		0x07, 0x00, 0x00, 0x00,                                                                          // 24  frameIndex = 7
		0x2A, 0x00, 0x00, 0x00,                                                                          // 28  fieldNumericId = 42
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F,                                                  // 32  simulationTime = 0.5
		0x05, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,                          // 40  dimensions = (5, 3, 6) CELL counts
		0x03, 0x00, 0x02, 0x00, 0x04, 0x00,                                                              // 52  brickSize = (3, 2, 4)
		0x02, 0x03, 0x00, 0x00,                                                                          // 58  componentCount = 2, dataType = 3, association = 0, codec = 0
		0x00, 0x00,                                                                                      // 62  reserved = 0
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 64  brickCount = 8
		0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 72  directoryOffset = 128
		0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 80  payloadOffset = 768
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 88  backgroundValue = float32[4] [0.0, 0.0, 0.0, 0.0]
		0xDE, 0x8D, 0x09, 0x4B,                                                                          // 104 headerCrc32c = 0x4B098DDE
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 108 reserved[20] = 0
		0x00, 0x00, 0x00, 0x00,                                                                          // 124 

		// ---- entry 0, offset 128: brick (0, 0, 0) ----
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 128  brickIndex = (0, 0, 0)
		0x03, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,                                                  // 140  validSize = (3, 2, 4), flags = 0
		0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,  // 148  payloadOffset = 768, compressed = 48, uncompressed = 48
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x42, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 164  componentMin = [0.0, 37.0, inf, inf]
		0x00, 0x00, 0x50, 0x42, 0x00, 0x00, 0xB2, 0x42, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 180  componentMax = [52.0, 89.0, -inf, -inf]
		0xEB, 0xB8, 0x5F, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 196  payloadCrc32c = 0xEA5FB8EB, reserved[8] = 0

		// ---- entry 1, offset 208: brick (1, 0, 0) ----
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 208  brickIndex = (1, 0, 0)
		0x02, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,                                                  // 220  validSize = (2, 2, 4), flags = 0
		0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,  // 228  payloadOffset = 816, compressed = 32, uncompressed = 32
		0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x0C, 0x42, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 244  componentMin = [3.0, 35.0, inf, inf]
		0x00, 0x00, 0x58, 0x42, 0x00, 0x00, 0xAC, 0x42, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 260  componentMax = [54.0, 86.0, -inf, -inf]
		0x33, 0xD3, 0x63, 0xBC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 276  payloadCrc32c = 0xBC63D333, reserved[8] = 0

		// ---- entry 2, offset 288: brick (0, 1, 0) ----
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 288  brickIndex = (0, 1, 0)
		0x03, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,                                                  // 300  validSize = (3, 1, 4), flags = 0
		0x50, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,  // 308  payloadOffset = 848, compressed = 24, uncompressed = 24
		0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 324  componentMin = [10.0, 32.0, inf, inf]
		0x00, 0x00, 0x64, 0x42, 0x00, 0x00, 0x9E, 0x42, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 340  componentMax = [57.0, 79.0, -inf, -inf]
		0x89, 0x3B, 0x20, 0x93, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 356  payloadCrc32c = 0x93203B89, reserved[8] = 0

		// ---- entry 3, offset 368: brick (1, 1, 0) ----
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 368  brickIndex = (1, 1, 0)
		0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,                                                  // 380  validSize = (2, 1, 4), flags = 0
		0x68, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,  // 388  payloadOffset = 872, compressed = 16, uncompressed = 16
		0x00, 0x00, 0x50, 0x41, 0x00, 0x00, 0xF0, 0x41, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 404  componentMin = [13.0, 30.0, inf, inf]
		0x00, 0x00, 0x6C, 0x42, 0x00, 0x00, 0x98, 0x42, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 420  componentMax = [59.0, 76.0, -inf, -inf]
		0x72, 0x53, 0xA4, 0x97, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 436  payloadCrc32c = 0x97A45372, reserved[8] = 0

		// ---- entry 4, offset 448: brick (0, 0, 1) ----
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,                          // 448  brickIndex = (0, 0, 1)
		0x03, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,                                                  // 460  validSize = (3, 2, 2), flags = 0
		0x78, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,  // 468  payloadOffset = 888, compressed = 24, uncompressed = 24
		0x00, 0x00, 0x70, 0x42, 0x00, 0x00, 0xE0, 0x40, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 484  componentMin = [60.0, 7.0, inf, inf]
		0x00, 0x00, 0xA4, 0x42, 0x00, 0x00, 0xE8, 0x41, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 500  componentMax = [82.0, 29.0, -inf, -inf]
		0xB1, 0xA9, 0x81, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 516  payloadCrc32c = 0x9681A9B1, reserved[8] = 0

		// ---- entry 5, offset 528: brick (1, 0, 1) ----
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,                          // 528  brickIndex = (1, 0, 1)
		0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,                                                  // 540  validSize = (2, 2, 2), flags = 0
		0x90, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,  // 548  payloadOffset = 912, compressed = 16, uncompressed = 16
		0x00, 0x00, 0x7C, 0x42, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 564  componentMin = [63.0, 5.0, inf, inf]
		0x00, 0x00, 0xA8, 0x42, 0x00, 0x00, 0xD0, 0x41, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 580  componentMax = [84.0, 26.0, -inf, -inf]
		0x1E, 0x49, 0xC2, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 596  payloadCrc32c = 0x09C2491E, reserved[8] = 0

		// ---- entry 6, offset 608: brick (0, 1, 1) ----
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,                          // 608  brickIndex = (0, 1, 1)
		0x03, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,                                                  // 620  validSize = (3, 1, 2), flags = 0
		0xA0, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,  // 628  payloadOffset = 928, compressed = 12, uncompressed = 12
		0x00, 0x00, 0x8C, 0x42, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 644  componentMin = [70.0, 2.0, inf, inf]
		0x00, 0x00, 0xAE, 0x42, 0x00, 0x00, 0x98, 0x41, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 660  componentMax = [87.0, 19.0, -inf, -inf]
		0xBE, 0xAF, 0x25, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 676  payloadCrc32c = 0x3F25AFBE, reserved[8] = 0

		// ---- entry 7, offset 688: brick (1, 1, 1) ----
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,                          // 688  brickIndex = (1, 1, 1)
		0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,                                                  // 700  validSize = (2, 1, 2), flags = 0
		0xAC, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,  // 708  payloadOffset = 940, compressed = 8, uncompressed = 8
		0x00, 0x00, 0x92, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 724  componentMin = [73.0, 0.0, inf, inf]
		0x00, 0x00, 0xB2, 0x42, 0x00, 0x00, 0x80, 0x41, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 740  componentMax = [89.0, 16.0, -inf, -inf]
		0x09, 0x6F, 0x48, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 756  payloadCrc32c = 0x86486F09, reserved[8] = 0

		// ---- payloads, offset 768 ----------------------------------
		0x00, 0x59, 0x01, 0x58, 0x02, 0x57, 0x05, 0x54, 0x06, 0x53, 0x07, 0x52, 0x0F, 0x4A, 0x10, 0x49,  // 768
		0x11, 0x48, 0x14, 0x45, 0x15, 0x44, 0x16, 0x43, 0x1E, 0x3B, 0x1F, 0x3A, 0x20, 0x39, 0x23, 0x36,  // 784
		0x24, 0x35, 0x25, 0x34, 0x2D, 0x2C, 0x2E, 0x2B, 0x2F, 0x2A, 0x32, 0x27, 0x33, 0x26, 0x34, 0x25,  // 800
		0x03, 0x56, 0x04, 0x55, 0x08, 0x51, 0x09, 0x50, 0x12, 0x47, 0x13, 0x46, 0x17, 0x42, 0x18, 0x41,  // 816
		0x21, 0x38, 0x22, 0x37, 0x26, 0x33, 0x27, 0x32, 0x30, 0x29, 0x31, 0x28, 0x35, 0x24, 0x36, 0x23,  // 832
		0x0A, 0x4F, 0x0B, 0x4E, 0x0C, 0x4D, 0x19, 0x40, 0x1A, 0x3F, 0x1B, 0x3E, 0x28, 0x31, 0x29, 0x30,  // 848
		0x2A, 0x2F, 0x37, 0x22, 0x38, 0x21, 0x39, 0x20, 0x0D, 0x4C, 0x0E, 0x4B, 0x1C, 0x3D, 0x1D, 0x3C,  // 864
		0x2B, 0x2E, 0x2C, 0x2D, 0x3A, 0x1F, 0x3B, 0x1E, 0x3C, 0x1D, 0x3D, 0x1C, 0x3E, 0x1B, 0x41, 0x18,  // 880
		0x42, 0x17, 0x43, 0x16, 0x4B, 0x0E, 0x4C, 0x0D, 0x4D, 0x0C, 0x50, 0x09, 0x51, 0x08, 0x52, 0x07,  // 896
		0x3F, 0x1A, 0x40, 0x19, 0x44, 0x15, 0x45, 0x14, 0x4E, 0x0B, 0x4F, 0x0A, 0x53, 0x06, 0x54, 0x05,  // 912
		0x46, 0x13, 0x47, 0x12, 0x48, 0x11, 0x55, 0x04, 0x56, 0x03, 0x57, 0x02, 0x49, 0x10, 0x4A, 0x0F,  // 928
		0x58, 0x01, 0x59, 0x00,                                                                          // 944
	};

	static_assert(sizeof(GoldenCvfBytes) == 948, "golden CVF fixture must stay 128 + 8*80 + 180 bytes");

	/**
	 * The SAME (5, 3, 6) geometry, but float32, ONE component, zlib, sparse:
	 * seven of the eight bricks are omitted and backgroundValue is NaN.
	 *
	 * Three things live here that the dense fixture cannot reach.
	 *
	 *  1. SECTION 4.4.5 ABSENCE. flags bit 0 is set and brickCount is 1, so
	 *     every voxel outside brick (0,0,0) must come back as the header's
	 *     backgroundValue - and that background is NaN, so the reader has to
	 *     move the bit pattern rather than compute with it (rule 1.7). A
	 *     background of 0.0 would be indistinguishable from a zero-filled
	 *     buffer, which is exactly the bug this is meant to catch.
	 *  2. SECTION 4.4.7 SENTINELS. Only component 0 is meaningful, so the
	 *     writer left componentMin/Max slots 1..3 at +inf / -inf. That is "no
	 *     valid data", NOT a range, and TryGetComponentRange must refuse it.
	 *  3. A REAL ZLIB STREAM. The dense fixture is codec none, so nothing there
	 *     exercises the inflate path or a payload CRC over compressed bytes.
	 *
	 * From `write_cvf(values=v, brick_size=(3,2,4), frame_index=3,
	 *                 field_numeric_id=9, simulation_time=1.25, dtype='float32',
	 *                 codec=3, background_value=nan, omit_background_bricks=True)`.
	 */
	const uint8 SparseCvfBytes[] = {
		// ---- header, offset 0 (format section 4.1) ----------------------
		0x43, 0x46, 0x44, 0x56, 0x4F, 0x4C, 0x31, 0x00,                                                  // 0   magic "CFDVOL1\0"
		0x80, 0x00, 0x00, 0x00,                                                                          // 8   headerBytes = 128
		0x01, 0x00, 0x00, 0x00,                                                                          // 12  majorVersion = 1, minorVersion = 0
		0x04, 0x03, 0x02, 0x01,                                                                          // 16  endianMarker = 0x01020304
		0x01, 0x00, 0x00, 0x00,                                                                          // 20  flags = 1
		0x03, 0x00, 0x00, 0x00,                                                                          // 24  frameIndex = 3
		0x09, 0x00, 0x00, 0x00,                                                                          // 28  fieldNumericId = 9
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x3F,                                                  // 32  simulationTime = 1.25
		0x05, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,                          // 40  dimensions = (5, 3, 6) CELL counts
		0x03, 0x00, 0x02, 0x00, 0x04, 0x00,                                                              // 52  brickSize = (3, 2, 4)
		0x01, 0x02, 0x00, 0x03,                                                                          // 58  componentCount = 1, dataType = 2, association = 0, codec = 3
		0x00, 0x00,                                                                                      // 62  reserved = 0
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 64  brickCount = 1
		0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 72  directoryOffset = 128
		0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                  // 80  payloadOffset = 208
		0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F,  // 88  backgroundValue = float32[4] [nan, nan, nan, nan]
		0x8E, 0xD6, 0x48, 0xB4,                                                                          // 104 headerCrc32c = 0xB448D68E
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 108 reserved[20] = 0
		0x00, 0x00, 0x00, 0x00,                                                                          // 124 

		// ---- entry 0, offset 128: brick (0, 0, 0) ----
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 128  brickIndex = (0, 0, 0)
		0x03, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00,                                                  // 140  validSize = (3, 2, 4), flags = 0
		0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,  // 148  payloadOffset = 208, compressed = 62, uncompressed = 96
		0x00, 0x00, 0xC0, 0xC0, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F, 0x00, 0x00, 0x80, 0x7F,  // 164  componentMin = [-6.0, inf, inf, inf]
		0x00, 0x00, 0x20, 0x40, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF, 0x00, 0x00, 0x80, 0xFF,  // 180  componentMax = [2.5, -inf, -inf, -inf]
		0x69, 0x30, 0xF7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                          // 196  payloadCrc32c = 0xC0F73069, reserved[8] = 0

		// ---- payloads, offset 208 ----------------------------------
		0x78, 0x9C, 0x25, 0x89, 0x01, 0x11, 0x00, 0x20, 0x08, 0x03, 0x89, 0x42, 0x13, 0x89, 0x62, 0x14,  // 208
		0x23, 0x18, 0xC1, 0x08, 0x36, 0x60, 0xD1, 0x7C, 0x84, 0xBB, 0x67, 0xBB, 0xBD, 0x59, 0xDD, 0x1A,  // 224
		0xBC, 0x00, 0x52, 0xE0, 0xD5, 0xC5, 0x9E, 0x5F, 0x9B, 0x2A, 0xB3, 0xFD, 0x62, 0x0F, 0xB5, 0x9F,  // 240
		0xE0, 0x6A, 0x2F, 0xF2, 0xA8, 0xFD, 0x85, 0xFD, 0xFD, 0x03, 0x26, 0x9D, 0x15, 0x99,              // 256
	};

	static_assert(sizeof(SparseCvfBytes) == 270, "sparse CVF fixture must stay 128 + 80 + 62 bytes");

	/** Where the sparse fixture's single payload starts. */
	constexpr int32 SparsePayloadOffset = 208;

	/* ---------------------------------------------------------------------- */
	/* Fixture surgery                                                          */
	/* ---------------------------------------------------------------------- */

	/** A modifiable copy, so a test can corrupt one byte without disturbing the fixture. */
	TArray<uint8> CopyOfCvf(const uint8* Bytes, int32 Count)
	{
		TArray<uint8> Copy;
		Copy.Append(Bytes, Count);
		return Copy;
	}

	TArray<uint8> CopyOfGolden()
	{
		return CopyOfCvf(GoldenCvfBytes, UE_ARRAY_COUNT(GoldenCvfBytes));
	}

	void WriteUInt8At(TArray<uint8>& Bytes, int32 Offset, uint8 Value)
	{
		Bytes[Offset] = Value;
	}

	void WriteUInt16At(TArray<uint8>& Bytes, int32 Offset, uint16 Value)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
	}

	void WriteCvfUInt32At(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
	}

	void WriteCvfUInt64At(TArray<uint8>& Bytes, int32 Offset, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
	}

	/** Write a float32's exact bit pattern, so a NaN or an infinity survives the edit. */
	void WriteFloatAt(TArray<uint8>& Bytes, int32 Offset, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		WriteCvfUInt32At(Bytes, Offset, Bits);
	}

	/**
	 * Re-seal the header CRC after a test has edited a header field.
	 *
	 * Section 4.1: CRC-32C over all 128 bytes with [104, 108) treated as ZERO -
	 * blanked, not skipped, so the four zero bytes still take part. Written out
	 * here rather than calling FCFDVizVolumeHeader::ComputeHeaderCrc so that the
	 * expected value comes from an independent restatement of the rule; using
	 * the reader's own function would make every CRC assertion below a tautology
	 * that a wrong-but-self-consistent implementation would pass.
	 *
	 * Without re-sealing, every field-mutation test would fail at the CRC and
	 * never reach the check it was written to exercise - a test that always
	 * fails for the same reason proves nothing about the field it names.
	 */
	void ResealCvfHeaderCrc(TArray<uint8>& Bytes)
	{
		FMemory::Memzero(Bytes.GetData() + HdrOffsetCrc, 4);
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData(), HdrBytes);
		WriteCvfUInt32At(Bytes, HdrOffsetCrc, Crc);
	}

	/** Byte offset of field `Field` inside directory entry `Entry` of the golden fixture. */
	constexpr int32 GoldenEntryField(int32 Entry, int32 Field)
	{
		return GoldenDirectoryOffset + Entry * CvfTestEntryBytes + Field;
	}

	/** Re-stamp a brick's payload CRC after its stored bytes or extent were edited. */
	void ResealBrickCrc(TArray<uint8>& Bytes, int32 Entry)
	{
		const int32 Base = GoldenDirectoryOffset + Entry * CvfTestEntryBytes;
		int64 Offset = 0;
		int64 Count = 0;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Offset |= static_cast<int64>(Bytes[Base + CvfTestEntryOffsetPayload + Index]) << (Index * 8);
		}
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Count |= static_cast<int64>(Bytes[Base + CvfTestEntryOffsetCompressed + Index]) << (Index * 8);
		}
		const uint32 Crc = CFDViz::Crc32C::Compute(Bytes.GetData() + Offset, Count);
		WriteCvfUInt32At(Bytes, Base + CvfTestEntryOffsetPayloadCrc, Crc);
	}

	/** float32 bit pattern of one decoded uint8 value, for asserting stored bytes exactly. */
	uint32 BitsOfFloat(float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	/**
	 * The value the golden fixture stores at (X, Y, Z) for component C.
	 *
	 * Restated from the generator's own formula rather than read back through
	 * the reader, so an assertion against it is a comparison with an
	 * independently known number instead of with whatever the reader produced.
	 */
	uint8 GoldenValueAt(int32 X, int32 Y, int32 Z, int32 Component)
	{
		const int32 Flat = X + 5 * Y + 15 * Z;
		return static_cast<uint8>(Component == 0 ? Flat : 89 - Flat);
	}
}

/* -------------------------------------------------------------------------- */
/* Layout - the header and the directory, field by field                        */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderLayoutTest,
	"FlowViz.CFDViz.VolumeReader.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderLayoutTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	const FCFDVizMemoryByteSource Source(
		TArrayView<const uint8>(GoldenCvfBytes, UE_ARRAY_COUNT(GoldenCvfBytes)), TEXT("golden.cvf"));

	FCFDVizVolumeReader Reader;
	const FCFDVizResult Result = Reader.Open(Source);
	if (!TestTrue(TEXT("the golden volume opens"), Result.IsOk()))
	{
		AddError(FString::Printf(TEXT("open failed: %s"), *Result.ToString()));
		return false;
	}

	// --- header fields, asserted against the section 4.1 offsets -------------
	const FCFDVizVolumeHeader& Header = Reader.GetHeader();
	TestEqual(TEXT("headerBytes @8"), static_cast<int64>(Header.HeaderBytes), static_cast<int64>(128));
	TestEqual(TEXT("majorVersion @12"), static_cast<int32>(Header.MajorVersion), 1);
	TestEqual(TEXT("minorVersion @14"), static_cast<int32>(Header.MinorVersion), 0);
	TestEqual(TEXT("endianMarker @16"), static_cast<int64>(Header.EndianMarker), static_cast<int64>(0x01020304));
	TestEqual(TEXT("flags @20"), static_cast<int64>(Header.Flags), static_cast<int64>(0));
	TestEqual(TEXT("frameIndex @24"), static_cast<int64>(Header.FrameIndex), static_cast<int64>(7));
	TestEqual(TEXT("fieldNumericId @28"), static_cast<int64>(Header.FieldNumericId), static_cast<int64>(42));
	TestEqual(TEXT("simulationTime @32"), Header.SimulationTime, 0.5);
	TestEqual(TEXT("dimensions @40 are CELL counts"), Header.Dimensions, FIntVector(5, 3, 6));
	TestEqual(TEXT("brickSize @52"), Header.BrickSize, FIntVector(3, 2, 4));
	TestEqual(TEXT("componentCount @58"), Header.ComponentCount, 2);
	TestTrue(TEXT("dataType @59 is uint8"), Header.DataType == ECFDVizDataType::UInt8);
	TestTrue(TEXT("association @60 is cell"), Header.Association == ECFDVizAssociation::Cell);
	TestTrue(TEXT("codec @61 is none"), Header.Codec == ECFDVizCodec::None);
	TestEqual(TEXT("brickCount @64"), Header.BrickCount, static_cast<int64>(8));
	TestEqual(TEXT("directoryOffset @72"), Header.DirectoryOffset, static_cast<int64>(GoldenDirectoryOffset));
	TestEqual(TEXT("payloadOffset @80"), Header.PayloadOffset, static_cast<int64>(GoldenPayloadOffset));
	TestEqual(TEXT("headerCrc32c @104"), static_cast<int64>(Header.HeaderCrc32C), static_cast<int64>(0x4B098DDE));
	TestFalse(TEXT("the sparse flag is clear on a dense file"), Header.IsSparse());

	// --- derived geometry ----------------------------------------------------
	// Cell association, so the value extent is the cell extent with no +1.
	TestEqual(TEXT("cell association applies no +1"), Header.GetValueCounts(), FIntVector(5, 3, 6));
	TestEqual(TEXT("5*3*6 values"), Header.GetValueCount(), static_cast<int64>(90));
	TestEqual(TEXT("one uint8 component is 1 byte"), Header.GetElementBytes(), 1);
	TestEqual(TEXT("a voxel is 2 components x 1 byte"), Header.GetVoxelBytes(), static_cast<int64>(2));
	TestEqual(TEXT("dense volume is 90 voxels x 2 bytes"), Header.GetDenseVolumeBytes(), static_cast<int64>(180));

	// ceil(5/3)=2, ceil(3/2)=2, ceil(6/4)=2. Every axis has an interior brick
	// AND a clipped edge brick, which is the whole point of this geometry.
	TestEqual(TEXT("brick grid is ceil per axis"), Header.GetBrickCounts(), FIntVector(2, 2, 2));
	TestEqual(TEXT("a dense file carries 2*2*2 bricks"), Header.GetTotalBrickCount(), static_cast<int64>(8));

	TestEqual(TEXT("the file size is measured, not taken from the header"),
		Reader.GetFileSize(), static_cast<int64>(sizeof(GoldenCvfBytes)));
	TestEqual(TEXT("the display path is carried through"), Reader.GetFilePath(), FString(TEXT("golden.cvf")));

	// --- the directory -------------------------------------------------------
	const TArray<FCFDVizBrickEntry>& Bricks = Reader.GetBricks();
	if (!TestEqual(TEXT("8 directory entries"), Bricks.Num(), 8))
	{
		return false;
	}
	TestEqual(TEXT("GetBrickCount counts entries, not the tiling"), Reader.GetBrickCount(), 8);
	TestEqual(TEXT("GetBrickEntries is the same array"), Reader.GetBrickEntries().Num(), 8);

	// Entry 0 is the only fully interior brick: 3x2x4 = 24 voxels x 2 bytes.
	// The payload offsets below are absolute and were written by the reference
	// implementation, so they also pin that the entries are in file order.
	TestEqual(TEXT("entry 0 brickIndex @0"), Bricks[0].BrickIndex, FIntVector(0, 0, 0));
	TestEqual(TEXT("entry 0 validSize @12 is the full brick"), Bricks[0].ValidSize, FIntVector(3, 2, 4));
	TestEqual(TEXT("entry 0 flags @18"), static_cast<int32>(Bricks[0].Flags), 0);
	TestEqual(TEXT("entry 0 payloadOffset @20"), Bricks[0].PayloadOffset, static_cast<int64>(768));
	TestEqual(TEXT("entry 0 compressedBytes @28"), Bricks[0].CompressedBytes, static_cast<int64>(48));
	TestEqual(TEXT("entry 0 uncompressedBytes @32"), Bricks[0].UncompressedBytes, static_cast<int64>(48));
	TestEqual(TEXT("entry 0 payloadCrc32c @68"), static_cast<int64>(Bricks[0].PayloadCrc32C), static_cast<int64>(0xEA5FB8EB));

	// Entry 7 is clipped on ALL THREE axes: 5-3=2, 3-2=1, 6-4=2. A reader that
	// padded edge bricks would want 3x2x4 here and read 48 bytes where 8 exist.
	TestEqual(TEXT("entry 7 brickIndex"), Bricks[7].BrickIndex, FIntVector(1, 1, 1));
	TestEqual(TEXT("entry 7 validSize is clipped on every axis"), Bricks[7].ValidSize, FIntVector(2, 1, 2));
	TestEqual(TEXT("entry 7 payloadOffset"), Bricks[7].PayloadOffset, static_cast<int64>(940));
	TestEqual(TEXT("entry 7 is 2*1*2 voxels x 2 components x 1 byte"),
		Bricks[7].UncompressedBytes, static_cast<int64>(8));
	TestEqual(TEXT("codec none: stored length equals decoded length"),
		Bricks[7].CompressedBytes, Bricks[7].UncompressedBytes);
	TestEqual(TEXT("entry 7 voxel count"), Bricks[7].GetVoxelCount(), static_cast<int64>(4));

	// The per-brick statistics are float32[4] at 36 and 52. Brick 7 covers
	// x in [3,5), y in [2,3), z in [4,6), so component 0 runs 73..89 and
	// component 1 - being 89 minus it - runs 0..16. Asserting both components
	// is what catches a reader that read componentMax from componentMin's
	// offset, or that read only slot 0 and reused it.
	float Min = 0.0f;
	float Max = 0.0f;
	if (TestTrue(TEXT("entry 7 component 0 has a range"), Bricks[7].TryGetComponentRange(0, Min, Max)))
	{
		TestEqual(TEXT("entry 7 componentMin[0] @36"), Min, 73.0f);
		TestEqual(TEXT("entry 7 componentMax[0] @52"), Max, 89.0f);
	}
	if (TestTrue(TEXT("entry 7 component 1 has a range"), Bricks[7].TryGetComponentRange(1, Min, Max)))
	{
		TestEqual(TEXT("entry 7 componentMin[1] @40"), Min, 0.0f);
		TestEqual(TEXT("entry 7 componentMax[1] @56"), Max, 16.0f);
	}

	// --- the coordinate index ------------------------------------------------
	// Every one of the eight bricks must be findable, and each must map to the
	// slot whose entry carries that coordinate. A lookup that hashed only two
	// axes would collide here and hand back the wrong slot.
	for (int32 Index = 0; Index < Bricks.Num(); ++Index)
	{
		const int32 Found = Reader.FindBrickByCoordinate(Bricks[Index].BrickIndex);
		TestEqual(TEXT("FindBrickByCoordinate round-trips every entry"), Found, Index);
	}
	// (2,0,0) is outside a 2x2x2 grid, so it was never stored.
	TestEqual(TEXT("a coordinate outside the grid is INDEX_NONE, not slot 0"),
		Reader.FindBrickByCoordinate(FIntVector(2, 0, 0)), INDEX_NONE);

	return true;
}

/* -------------------------------------------------------------------------- */
/* Decoded values - ReadBrick, ReadVoxel and ReadDense against hand-computed    */
/* numbers                                                                      */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderValuesTest,
	"FlowViz.CFDViz.VolumeReader.Values",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderValuesTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	const FCFDVizMemoryByteSource Source(
		TArrayView<const uint8>(GoldenCvfBytes, UE_ARRAY_COUNT(GoldenCvfBytes)), TEXT("golden.cvf"));

	FCFDVizVolumeReader Reader;
	if (!TestTrue(TEXT("the golden volume opens"), Reader.Open(Source).IsOk()))
	{
		return false;
	}

	// --- ReadDense: every one of the 90 voxels, against the generator formula -
	//
	// Not "the volume decoded" but "byte N is exactly this number". The layout
	// under test is X fastest, then Y, then Z, components interleaved (sections
	// 4.4.1 / 4.4.2), reassembled from eight bricks of three different clipped
	// shapes. An axis transposition, a row shear from treating an edge brick as
	// a full cube, or a component swap all land on a different byte here -
	// which is only true because no two axes of this fixture are the same
	// length and the two components differ at every voxel.
	TArray<uint8> Dense;
	const FCFDVizResult DenseResult = Reader.ReadDense(Dense);
	if (!TestTrue(TEXT("ReadDense succeeds"), DenseResult.IsOk()))
	{
		AddError(FString::Printf(TEXT("ReadDense failed: %s"), *DenseResult.ToString()));
		return false;
	}
	if (!TestEqual(TEXT("dense volume is 5*3*6*2 bytes"), Dense.Num(), 180))
	{
		return false;
	}

	int32 Mismatches = 0;
	for (int32 Z = 0; Z < 6; ++Z)
	{
		for (int32 Y = 0; Y < 3; ++Y)
		{
			for (int32 X = 0; X < 5; ++X)
			{
				for (int32 Component = 0; Component < 2; ++Component)
				{
					const int32 ByteIndex = ((Z * 3 + Y) * 5 + X) * 2 + Component;
					const uint8 Expected = GoldenValueAt(X, Y, Z, Component);
					if (Dense[ByteIndex] != Expected)
					{
						++Mismatches;
						if (Mismatches <= 5)
						{
							AddError(FString::Printf(
								TEXT("dense voxel (%d, %d, %d) component %d: expected %u, got %u"),
								X, Y, Z, Component, Expected, Dense[ByteIndex]));
						}
					}
				}
			}
		}
	}
	TestEqual(TEXT("every dense voxel matches the generator formula"), Mismatches, 0);

	// --- ReadVoxel agrees with ReadDense, byte for byte ----------------------
	//
	// These are separate code paths that share the tiling arithmetic by
	// construction; the section 9 known_values bridge calls ReadVoxel, so a
	// ReadVoxel that disagreed with ReadDense would make a case pass or fail
	// depending on which one a caller happened to use. The coordinates below
	// are chosen to land in different bricks: the interior brick, the
	// triple-clipped corner brick, and one brick clipped on a single axis.
	const FIntVector Probes[] = {
		FIntVector(0, 0, 0),  // brick (0,0,0), first voxel of the volume
		FIntVector(2, 1, 3),  // brick (0,0,0), last voxel of the interior brick
		FIntVector(3, 0, 0),  // brick (1,0,0), clipped on X only
		FIntVector(0, 2, 0),  // brick (0,1,0), clipped on Y only
		FIntVector(0, 0, 4),  // brick (0,0,1), clipped on Z only
		FIntVector(4, 2, 5),  // brick (1,1,1), last voxel of the volume
	};
	for (const FIntVector& Probe : Probes)
	{
		TArray<uint8> Voxel;
		const FCFDVizResult VoxelResult = Reader.ReadVoxel(Probe.X, Probe.Y, Probe.Z, Voxel);
		if (!TestTrue(TEXT("ReadVoxel succeeds inside the extent"), VoxelResult.IsOk()))
		{
			AddError(FString::Printf(TEXT("ReadVoxel failed: %s"), *VoxelResult.ToString()));
			continue;
		}
		if (!TestEqual(TEXT("a voxel is 2 bytes"), Voxel.Num(), 2))
		{
			continue;
		}
		for (int32 Component = 0; Component < 2; ++Component)
		{
			const int32 ByteIndex = ((Probe.Z * 3 + Probe.Y) * 5 + Probe.X) * 2 + Component;
			TestEqual(
				*FString::Printf(TEXT("ReadVoxel(%d, %d, %d)[%d] matches the formula"),
					Probe.X, Probe.Y, Probe.Z, Component),
				static_cast<int32>(Voxel[Component]),
				static_cast<int32>(GoldenValueAt(Probe.X, Probe.Y, Probe.Z, Component)));
			TestEqual(
				*FString::Printf(TEXT("ReadVoxel(%d, %d, %d)[%d] matches ReadDense"),
					Probe.X, Probe.Y, Probe.Z, Component),
				static_cast<int32>(Voxel[Component]),
				static_cast<int32>(Dense[ByteIndex]));
		}
	}

	// NEGATIVE CONTROL for the extent check. Cell association means the legal
	// range is exactly [0,5) x [0,3) x [0,6); one past on any axis must be
	// refused rather than clamped or wrapped into a neighbouring brick.
	{
		TArray<uint8> Voxel;
		const FCFDVizResult Past = Reader.ReadVoxel(5, 0, 0, Voxel);
		TestFalse(TEXT("ReadVoxel(5,0,0) is outside a 5-wide extent"), Past.IsOk());
		TestTrue(TEXT("...and says IndexOutOfRange"), Past.Error == ECFDVizError::IndexOutOfRange);
		TestEqual(TEXT("...leaving nothing that could pass for data"), Voxel.Num(), 0);

		TestFalse(TEXT("ReadVoxel(0,3,0) is outside a 3-deep extent"), Reader.ReadVoxel(0, 3, 0, Voxel).IsOk());
		TestFalse(TEXT("ReadVoxel(0,0,6) is outside a 6-tall extent"), Reader.ReadVoxel(0, 0, 6, Voxel).IsOk());
		TestFalse(TEXT("a negative index is refused too"), Reader.ReadVoxel(-1, 0, 0, Voxel).IsOk());
	}

	// --- ReadBrick: the triple-clipped corner brick, voxel by voxel ----------
	//
	// Brick 7 is at (1,1,1), so its origin is (3, 2, 4) and it stores 2x1x2
	// voxels - clipped on every axis. Indexing it as a full 3x2x4 cube would
	// read 48 bytes from an 8-byte payload; indexing it with the right count
	// but the wrong origin would return the wrong (and still plausible) values.
	{
		TArray<uint8> Brick;
		const FCFDVizResult BrickResult = Reader.ReadBrick(7, Brick);
		if (TestTrue(TEXT("ReadBrick(7) succeeds"), BrickResult.IsOk()))
		{
			if (TestEqual(TEXT("the corner brick is 2*1*2*2 bytes"), Brick.Num(), 8))
			{
				for (int32 LocalZ = 0; LocalZ < 2; ++LocalZ)
				{
					for (int32 LocalX = 0; LocalX < 2; ++LocalX)
					{
						for (int32 Component = 0; Component < 2; ++Component)
						{
							const int32 ByteIndex = (LocalZ * 2 + LocalX) * 2 + Component;
							TestEqual(
								*FString::Printf(TEXT("brick 7 local (%d, 0, %d)[%d]"), LocalX, LocalZ, Component),
								static_cast<int32>(Brick[ByteIndex]),
								static_cast<int32>(GoldenValueAt(3 + LocalX, 2, 4 + LocalZ, Component)));
						}
					}
				}
			}
		}

		// NEGATIVE CONTROL: BrickIndex is a directory slot, not a coordinate.
		TArray<uint8> Rejected;
		const FCFDVizResult OutOfRange = Reader.ReadBrick(8, Rejected);
		TestFalse(TEXT("ReadBrick(8) is past the 8 entries"), OutOfRange.IsOk());
		TestTrue(TEXT("...and says IndexOutOfRange"), OutOfRange.Error == ECFDVizError::IndexOutOfRange);
		TestFalse(TEXT("ReadBrick(-1) is refused"), Reader.ReadBrick(-1, Rejected).IsOk());
	}

	// --- integrity, without decoding anything --------------------------------
	TestTrue(TEXT("every CRC in a clean file verifies"), Reader.VerifyAllCrcs().IsOk());
	TestTrue(TEXT("the header CRC verifies on its own"), Reader.VerifyHeaderCrc().IsOk());
	TArray<int32> Failed;
	TestTrue(TEXT("VerifyAllPayloadCrcs passes on a clean file"), Reader.VerifyAllPayloadCrcs(Failed).IsOk());
	TestEqual(TEXT("...with no bad bricks listed"), Failed.Num(), 0);

	// --- a closed reader refuses every read ----------------------------------
	Reader.Close();
	TestFalse(TEXT("IsOpen is false after Close"), Reader.IsOpen());
	TArray<uint8> Ignored;
	TestFalse(TEXT("ReadDense on a closed reader fails"), Reader.ReadDense(Ignored).IsOk());
	TestFalse(TEXT("ReadVoxel on a closed reader fails"), Reader.ReadVoxel(0, 0, 0, Ignored).IsOk());
	TestFalse(TEXT("ReadBrick on a closed reader fails"), Reader.ReadBrick(0, Ignored).IsOk());
	TestFalse(TEXT("VerifyAllCrcs on a closed reader fails"), Reader.VerifyAllCrcs().IsOk());

	return true;
}

/* -------------------------------------------------------------------------- */
/* Header CRC - the blanking rule, rejection, and the escape hatch              */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderHeaderCrcTest,
	"FlowViz.CFDViz.VolumeReader.HeaderCrc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderHeaderCrcTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	// --- the blanking rule, stated independently -----------------------------
	//
	// Section 4.1: CRC-32C over all 128 bytes with [104, 108) treated as ZERO.
	// The four zero bytes take PART in the checksum; they are not skipped.
	// The expected value here is computed from that restatement, and it equals
	// the value the Python writer stamped into the fixture - so the check is
	// against two independent sources, not against the function under test.
	{
		uint8 Blanked[HdrBytes];
		FMemory::Memcpy(Blanked, GoldenCvfBytes, HdrBytes);
		FMemory::Memzero(Blanked + HdrOffsetCrc, 4);
		const uint32 Restated = CFDViz::Crc32C::Compute(Blanked, HdrBytes);

		TestEqual(TEXT("ComputeHeaderCrc blanks [104,108) rather than skipping it"),
			static_cast<int64>(FCFDVizVolumeHeader::ComputeHeaderCrc(
				TArrayView<const uint8>(GoldenCvfBytes, HdrBytes))),
			static_cast<int64>(Restated));
		TestEqual(TEXT("...and matches the CRC the reference writer stored"),
			static_cast<int64>(Restated), static_cast<int64>(0x4B098DDE));

		// NEGATIVE CONTROL for "blanked, not skipped": a CRC computed over the
		// 124 bytes OUTSIDE the field is a different, self-consistent answer
		// that no other implementation would agree with. If these matched, the
		// zero bytes would not be participating and the rule would be unstated.
		uint32 Skipped = CFDViz::Crc32C::Compute(GoldenCvfBytes, HdrOffsetCrc);
		Skipped = CFDViz::Crc32C::Compute(GoldenCvfBytes + HdrOffsetCrc + 4, HdrBytes - HdrOffsetCrc - 4, Skipped);
		TestNotEqual(TEXT("skipping the four bytes gives a different answer, so they really do participate"),
			static_cast<int64>(Skipped), static_cast<int64>(Restated));

		// A span shorter than the header has no defined CRC, and returning some
		// partial checksum would let a truncated header validate.
		TestEqual(TEXT("a short span has no CRC"),
			static_cast<int64>(FCFDVizVolumeHeader::ComputeHeaderCrc(
				TArrayView<const uint8>(GoldenCvfBytes, HdrBytes - 1))),
			static_cast<int64>(0));
	}

	// --- a good CRC validates ------------------------------------------------
	{
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(GoldenCvfBytes, HdrBytes), Header, /*bVerifyCrc=*/true);
		TestTrue(TEXT("the untouched header passes CRC verification"), Result.IsOk());
	}

	// --- ...and a corrupted one is REJECTED ----------------------------------
	//
	// Two mutations, because they fail for different reasons. Flipping a byte
	// the CRC covers must be caught by the recomputation; flipping the stored
	// CRC itself must be caught by the comparison. A reader that computed the
	// CRC over the wrong range would still reject the second and pass the
	// first, so both are needed.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrOffsetFrameIndex] ^= 0x01;	// frameIndex 7 -> 6; CRC not re-sealed
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header, /*bVerifyCrc=*/true);
		TestFalse(TEXT("a flipped bit in a covered field is rejected"), Result.IsOk());
		TestTrue(TEXT("...as HeaderCrcMismatch"), Result.Error == ECFDVizError::HeaderCrcMismatch);
		TestEqual(TEXT("...naming byte 104, where the CRC lives"), Result.ByteOffset, static_cast<int64>(HdrOffsetCrc));
	}
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrOffsetCrc] ^= 0xFF;	// corrupt the stored CRC itself
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header, /*bVerifyCrc=*/true);
		TestFalse(TEXT("a corrupted stored CRC is rejected"), Result.IsOk());
		TestTrue(TEXT("...as HeaderCrcMismatch"), Result.Error == ECFDVizError::HeaderCrcMismatch);
	}
	{
		// The reserved tail is inside the checksummed range even though nothing
		// reads it. A reader that checksummed only the fields it parses would
		// miss this.
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrBytes - 1] = 0x00;	// already zero: no change, must still pass
		FCFDVizVolumeHeader Unchanged;
		TestTrue(TEXT("a no-op edit still passes"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Unchanged).IsOk());
	}

	// --- bVerifyCrc = false skips the check, and ONLY the check --------------
	//
	// The section 10 validator has to open a file with a corrupt header CRC in
	// order to report it. What it must not do is skip the structural checks: a
	// file that is not a CVF at all still has to be refused. The second half of
	// this block is the control that proves the flag is narrow.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrOffsetCrc] ^= 0xFF;

		FCFDVizVolumeHeader Header;
		const FCFDVizResult Skipped = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header, /*bVerifyCrc=*/false);
		TestTrue(TEXT("bVerifyCrc=false accepts a header with a bad CRC"), Skipped.IsOk());
		// The field is still parsed and retained, so a diagnostic can show what
		// was on disk - it is simply not compared.
		TestEqual(TEXT("...and the corrupt CRC is still reported as stored"),
			static_cast<int64>(Header.HeaderCrc32C),
			static_cast<int64>(0x4B098DDE ^ 0x000000FF));
		TestEqual(TEXT("...and the rest of the header still parses"), Header.Dimensions, FIntVector(5, 3, 6));

		// NEGATIVE CONTROL: with the same flag, a broken magic is still refused.
		Bytes[HdrOffsetMagic] = 'X';
		const FCFDVizResult StillRefused = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header, /*bVerifyCrc=*/false);
		TestFalse(TEXT("bVerifyCrc=false does not disable the structural checks"), StillRefused.IsOk());
		TestTrue(TEXT("...a non-CVF file still reports BadMagic"), StillRefused.Error == ECFDVizError::BadMagic);
	}

	// --- the same rule, through Open and VerifyHeaderCrc ---------------------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrOffsetFieldNumericId] ^= 0x10;	// CRC deliberately left stale

		const FCFDVizMemoryByteSource BadSource(Bytes, TEXT("corrupt.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Refused = Reader.Open(BadSource);
		TestFalse(TEXT("Open refuses a file whose header CRC does not match"), Refused.IsOk());
		TestTrue(TEXT("...as HeaderCrcMismatch"), Refused.Error == ECFDVizError::HeaderCrcMismatch);
		TestEqual(TEXT("...naming the file"), Refused.FilePath, FString(TEXT("corrupt.cvf")));
		TestFalse(TEXT("...leaving the reader closed, never half-open"), Reader.IsOpen());

		// The validator path: opened with the check off, VerifyHeaderCrc still
		// reports the corruption. If it consulted the cached header instead of
		// re-reading the bytes it would compare a stale value against itself and
		// report success.
		const FCFDVizResult Opened = Reader.Open(BadSource, /*bVerifyHeaderCrc=*/false);
		if (TestTrue(TEXT("bVerifyHeaderCrc=false opens the corrupt file"), Opened.IsOk()))
		{
			TestFalse(TEXT("VerifyHeaderCrc still reports it afterwards"), Reader.VerifyHeaderCrc().IsOk());
			TestTrue(TEXT("...as HeaderCrcMismatch"),
				Reader.VerifyHeaderCrc().Error == ECFDVizError::HeaderCrcMismatch);
			TestFalse(TEXT("VerifyAllCrcs reports it too"), Reader.VerifyAllCrcs().IsOk());
			// The directory is untouched, so the payload CRCs are all fine -
			// which is what makes the failure above specifically the header's.
			TArray<int32> FailedBricks;
			TestTrue(TEXT("the payload CRCs are unaffected"), Reader.VerifyAllPayloadCrcs(FailedBricks).IsOk());
		}
	}

	// --- re-sealing works, so later mutation tests reach their real check ----
	//
	// Every field-mutation test below edits a header field and re-seals. If
	// re-sealing were broken they would all fail at the CRC instead of at the
	// check they name, and would prove nothing. This asserts the harness.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetFrameIndex, 4242);
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		if (TestTrue(TEXT("a re-sealed header passes the CRC"), Result.IsOk()))
		{
			TestEqual(TEXT("...and frameIndex really changed"),
				static_cast<int64>(Header.FrameIndex), static_cast<int64>(4242));
			// Nothing adjacent moved: this is the differential half of the
			// check, and it is what a plain "frameIndex parses to 4242" misses
			// when two fields are read from each other's offsets.
			TestEqual(TEXT("...and fieldNumericId did not"),
				static_cast<int64>(Header.FieldNumericId), static_cast<int64>(42));
			TestEqual(TEXT("...and simulationTime did not"), Header.SimulationTime, 0.5);
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Field-offset probes - one field moves, and only that field                   */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderOffsetProbeTest,
	"FlowViz.CFDViz.VolumeReader.OffsetProbes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderOffsetProbeTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	// A DIFFERENTIAL test, not an assertion of parsed values.
	//
	// "dimensionY parses to 3" passes just as happily when dimensionY and
	// dimensionZ are read from each other's offsets, if the fixture happens to
	// be symmetric. Each probe below changes ONE field in the file and asserts
	// that exactly that field changed in the parse, so a transposition shows up
	// as the wrong field moving - which a value assertion cannot see.

	// --- the three dimensions are three distinct uint32s at 40, 44, 48 -------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetDimensions + 4, 11);	// dimensionY only
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		if (TestTrue(TEXT("the probe header parses"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk()))
		{
			TestEqual(TEXT("dimensionY @44 moved"), Header.Dimensions.Y, 11);
			TestEqual(TEXT("dimensionX @40 did not"), Header.Dimensions.X, 5);
			TestEqual(TEXT("dimensionZ @48 did not"), Header.Dimensions.Z, 6);
			// The brick size sits immediately after and must not have been
			// dragged along by a four-byte overread.
			TestEqual(TEXT("brickSize @52 did not"), Header.BrickSize, FIntVector(3, 2, 4));
		}
	}

	// --- brickSize is uint16[3] at 52/54/56, NOT uint32[3] -------------------
	//
	// Writing 0x0101 into brickSizeY leaves the byte at 55 as 0x01. A reader
	// that read brickSize as uint32 would fold that byte into brickSizeX or
	// produce a wildly different Y; asserting all three catches either.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, HdrOffsetBrickSize + 2, 0x0101);
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		if (TestTrue(TEXT("the probe header parses"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk()))
		{
			TestEqual(TEXT("brickSizeY @54 moved"), Header.BrickSize.Y, 0x0101);
			TestEqual(TEXT("brickSizeX @52 did not"), Header.BrickSize.X, 3);
			TestEqual(TEXT("brickSizeZ @56 did not"), Header.BrickSize.Z, 4);
		}
	}

	// --- the four enum bytes at 58/59/60/61 are one byte each ----------------
	//
	// componentCount, dataType, association and codec are adjacent single
	// bytes. Changing association alone must not disturb its neighbours; a
	// reader that read any of them as a uint16 or from the wrong offset would
	// see the change on the wrong field.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt8At(Bytes, HdrOffsetAssociation, 1);	// cell -> point
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		if (TestTrue(TEXT("the probe header parses"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk()))
		{
			TestTrue(TEXT("association @60 moved to point"), Header.Association == ECFDVizAssociation::Point);
			TestEqual(TEXT("componentCount @58 did not"), Header.ComponentCount, 2);
			TestTrue(TEXT("dataType @59 did not"), Header.DataType == ECFDVizDataType::UInt8);
			TestTrue(TEXT("codec @61 did not"), Header.Codec == ECFDVizCodec::None);

			// THE +1. Point association puts values on cell corners, so the
			// value extent is one larger per axis - and everything downstream
			// follows from it. This is the half-voxel shift the format calls
			// the single most common visualisation error.
			TestEqual(TEXT("point association adds one value per axis"),
				Header.GetValueCounts(), FIntVector(6, 4, 7));
			TestEqual(TEXT("...so the value count is 6*4*7, not 5*3*6"),
				Header.GetValueCount(), static_cast<int64>(168));
			// ceil(6/3)=2, ceil(4/2)=2, ceil(7/4)=2: the brick grid is unchanged
			// in shape but the edge bricks now clip differently.
			TestEqual(TEXT("...and the tiling covers the larger extent"),
				Header.GetBrickCounts(), FIntVector(2, 2, 2));
			TestEqual(TEXT("...with a full-depth edge brick on Y where the cell field had a partial one"),
				Header.GetValidSizeForBrick(FIntVector(0, 1, 0)), FIntVector(3, 2, 4));
		}
	}

	// --- the three uint64s at 64, 72, 80 -------------------------------------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetDirectoryOffset, 4096);
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		if (TestTrue(TEXT("the probe header parses"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk()))
		{
			TestEqual(TEXT("directoryOffset @72 moved"), Header.DirectoryOffset, static_cast<int64>(4096));
			TestEqual(TEXT("brickCount @64 did not"), Header.BrickCount, static_cast<int64>(8));
			TestEqual(TEXT("payloadOffset @80 did not"), Header.PayloadOffset, static_cast<int64>(768));
		}
	}

	// --- backgroundValue is float32[4] at 88, and it is DATA ----------------
	//
	// Slot 2 is written, so a reader that used the wrong stride or started the
	// array at the wrong offset would see the change in slot 1 or 3, or would
	// drag it into headerCrc32c at 104.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteFloatAt(Bytes, HdrOffsetBackground + 2 * 4, -2.5f);
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		if (TestTrue(TEXT("the probe header parses"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk()))
		{
			TestEqual(TEXT("backgroundValue[2] @96 moved"), Header.BackgroundValue[2], -2.5f);
			TestEqual(TEXT("backgroundValue[1] @92 did not"), Header.BackgroundValue[1], 0.0f);
			TestEqual(TEXT("backgroundValue[3] @100 did not"), Header.BackgroundValue[3], 0.0f);
		}
	}

	// --- the same treatment for a directory entry ----------------------------
	//
	// validSize is uint16[3] at 12/14/16 and flags is the uint16 at 18. Only
	// entry 0 is edited, and only its Y component, so a reader that read
	// validSize as uint32[3] would swallow validSizeZ, and one that started the
	// array a field early would report the change on brickIndexZ.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetValidSize + 2), 999);

		FCFDVizBrickEntry Entry;
		const FCFDVizResult Result = FCFDVizBrickEntry::Parse(
			TArrayView<const uint8>(Bytes.GetData() + GoldenDirectoryOffset, CvfTestEntryBytes), Entry);
		if (TestTrue(TEXT("the probe entry parses"), Result.IsOk()))
		{
			TestEqual(TEXT("validSizeY @14 moved"), Entry.ValidSize.Y, 999);
			TestEqual(TEXT("validSizeX @12 did not"), Entry.ValidSize.X, 3);
			TestEqual(TEXT("validSizeZ @16 did not"), Entry.ValidSize.Z, 4);
			TestEqual(TEXT("flags @18 did not"), static_cast<int32>(Entry.Flags), 0);
			TestEqual(TEXT("brickIndex @0 did not"), Entry.BrickIndex, FIntVector(0, 0, 0));
			TestEqual(TEXT("payloadOffset @20 did not"), Entry.PayloadOffset, static_cast<int64>(768));
		}
	}

	// --- compressedBytes @28 and uncompressedBytes @32 are two uint32s -------
	//
	// They are equal in this fixture (codec none), which is exactly the
	// condition under which a transposition is invisible to a value assertion.
	// Moving one and asserting the other stayed is the only way to tell.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetCompressed), 37);

		FCFDVizBrickEntry Entry;
		if (TestTrue(TEXT("the probe entry parses"),
			FCFDVizBrickEntry::Parse(
				TArrayView<const uint8>(Bytes.GetData() + GoldenDirectoryOffset, CvfTestEntryBytes), Entry).IsOk()))
		{
			TestEqual(TEXT("compressedBytes @28 moved"), Entry.CompressedBytes, static_cast<int64>(37));
			TestEqual(TEXT("uncompressedBytes @32 did not"), Entry.UncompressedBytes, static_cast<int64>(48));
			TestEqual(TEXT("payloadCrc32c @68 did not"),
				static_cast<int64>(Entry.PayloadCrc32C), static_cast<int64>(0xEA5FB8EB));
		}
	}

	// --- componentMin @36 and componentMax @52 are two distinct float32[4] ---
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteFloatAt(Bytes, GoldenEntryField(0, CvfTestEntryOffsetComponentMin + 4), -12.5f);	// min[1]
		WriteFloatAt(Bytes, GoldenEntryField(0, CvfTestEntryOffsetComponentMax + 4), 77.25f);	// max[1]

		FCFDVizBrickEntry Entry;
		if (TestTrue(TEXT("the probe entry parses"),
			FCFDVizBrickEntry::Parse(
				TArrayView<const uint8>(Bytes.GetData() + GoldenDirectoryOffset, CvfTestEntryBytes), Entry).IsOk()))
		{
			float Min = 0.0f;
			float Max = 0.0f;
			if (TestTrue(TEXT("component 1 has a range"), Entry.TryGetComponentRange(1, Min, Max)))
			{
				TestEqual(TEXT("componentMin[1] @40 moved"), Min, -12.5f);
				TestEqual(TEXT("componentMax[1] @56 moved"), Max, 77.25f);
			}
			// Component 0 is at 36 and 52 and was not touched. Its values are
			// the reference writer's, so this pins the array bases as well.
			if (TestTrue(TEXT("component 0 still has its own range"), Entry.TryGetComponentRange(0, Min, Max)))
			{
				TestEqual(TEXT("componentMin[0] @36 did not move"), Min, 0.0f);
				TestEqual(TEXT("componentMax[0] @52 did not move"), Max, 52.0f);
			}
		}
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Truncation at every structural boundary                                      */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderTruncationTest,
	"FlowViz.CFDViz.VolumeReader.Truncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderTruncationTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	// A CVF has three structural boundaries: the fixed header ends at 128, the
	// directory ends at 128 + 80*brickCount, and the payloads end at the file
	// end. A file cut anywhere must fail cleanly - reporting where - rather
	// than reading past the buffer it was handed. Every case below is the SAME
	// bytes with a shorter length, so nothing but the size differs.

	const auto OpenTruncated = [this](int32 Length) -> FCFDVizResult
	{
		const FCFDVizMemoryByteSource Source(
			TArrayView<const uint8>(GoldenCvfBytes, Length), TEXT("cut.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		// Whatever happened, the reader must not be left usable.
		TestFalse(TEXT("a rejected file leaves the reader closed"), Reader.IsOpen());
		return Result;
	};

	// --- short of the 128-byte header ----------------------------------------
	{
		// Every length from 0 to 127 fails, not just the obvious ones. 8 is
		// exactly the magic, 127 is one byte short - the case an off-by-one in
		// the length check would let through.
		const int32 ShortLengths[] = { 0, 1, 8, 64, 103, 127 };
		for (int32 Length : ShortLengths)
		{
			const FCFDVizResult Result = OpenTruncated(Length);
			TestFalse(*FString::Printf(TEXT("a %d-byte file is not a CVF"), Length), Result.IsOk());
			TestTrue(*FString::Printf(TEXT("...%d bytes reports FileTooSmall"), Length),
				Result.Error == ECFDVizError::FileTooSmall);
		}

		// The same rule at the Parse entry point, which has no file behind it.
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(GoldenCvfBytes, HdrBytes - 1), Header);
		TestFalse(TEXT("Parse refuses 127 bytes"), Result.IsOk());
		TestTrue(TEXT("...as FileTooSmall"), Result.Error == ECFDVizError::FileTooSmall);

		// POSITIVE CONTROL: exactly 128 bytes is enough for the header alone.
		// Without this, the assertions above would pass for a reader that
		// refused every input.
		TestTrue(TEXT("exactly 128 bytes parses as a header"),
			FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(GoldenCvfBytes, HdrBytes), Header).IsOk());
	}

	// --- mid-directory -------------------------------------------------------
	//
	// The directory is 8 entries of 80 bytes at offset 128, so it ends at 768.
	// A file that stops inside it must be refused BEFORE brickCount is
	// multiplied into an allocation - the whole reason the count is bounded
	// against the file size first.
	//
	// payloadOffset is lowered to 128 in these copies, because a plain
	// truncation trips the earlier payloadOffset-inside-the-file check and
	// would never reach the directory bound. That earlier rejection is correct
	// and is asserted separately below; the point of THIS block is the
	// directory bound specifically, so the fixture is arranged to reach it.
	{
		const int32 CutLengths[] = {
			HdrBytes,			 // header only: the directory is entirely absent
			HdrBytes + 1,		 // one byte into the first entry
			HdrBytes + CvfTestEntryBytes,	 // exactly one whole entry of the eight
			GoldenPayloadOffset - 1,	 // one byte short of a complete directory
		};
		for (int32 Length : CutLengths)
		{
			TArray<uint8> Bytes = CopyOfGolden();
			WriteCvfUInt64At(Bytes, HdrOffsetPayloadOffset, HdrBytes);
			ResealCvfHeaderCrc(Bytes);
			Bytes.SetNum(Length);

			const FCFDVizMemoryByteSource Source(Bytes, TEXT("cut-dir.cvf"));
			FCFDVizVolumeReader Reader;
			const FCFDVizResult Result = Reader.Open(Source);

			TestFalse(*FString::Printf(TEXT("a file cut at %d has no complete directory"), Length), Result.IsOk());
			TestTrue(*FString::Printf(TEXT("...%d reports DirectoryOutOfBounds"), Length),
				Result.Error == ECFDVizError::DirectoryOutOfBounds);
			TestEqual(*FString::Printf(TEXT("...%d names directoryOffset @72"), Length),
				Result.ByteOffset, static_cast<int64>(HdrOffsetDirectoryOffset));
			TestFalse(TEXT("...leaving the reader closed"), Reader.IsOpen());
		}

		// A plain truncation into the directory region is ALSO refused, just
		// earlier and for a different stated reason: payloadOffset 768 is no
		// longer inside the file. Asserted here so the check order above is a
		// deliberate arrangement rather than a gap.
		const FCFDVizResult Plain = OpenTruncated(HdrBytes + CvfTestEntryBytes);
		TestFalse(TEXT("a plain truncation into the directory is refused too"), Plain.IsOk());
		TestTrue(TEXT("...as InvalidHeader, from payloadOffset @80"), Plain.Error == ECFDVizError::InvalidHeader);
		TestEqual(TEXT("...naming byte 80"), Plain.ByteOffset, static_cast<int64>(HdrOffsetPayloadOffset));
	}

	// --- mid-payload ---------------------------------------------------------
	//
	// The directory is complete but a brick's bytes are not all there. This is
	// caught at Open, from the entry's own offset and length against the real
	// file size, so no later call can reach a read that runs off the end.
	{
		const int32 CutLengths[] = {
			GoldenPayloadOffset,		 // directory complete, zero payload bytes
			GoldenPayloadOffset + 47,	 // one byte short of brick 0
			947,				 // one byte short of the whole file
		};
		for (int32 Length : CutLengths)
		{
			const FCFDVizResult Result = OpenTruncated(Length);
			TestFalse(*FString::Printf(TEXT("a file cut at %d is missing payload bytes"), Length), Result.IsOk());
			TestTrue(*FString::Printf(TEXT("...%d reports PayloadOutOfBounds"), Length),
				Result.Error == ECFDVizError::PayloadOutOfBounds);
		}

		// POSITIVE CONTROL: the full 948 bytes open. This is what makes each
		// assertion above a statement about the missing byte rather than about
		// a reader that refuses everything.
		const FCFDVizMemoryByteSource Whole(
			TArrayView<const uint8>(GoldenCvfBytes, UE_ARRAY_COUNT(GoldenCvfBytes)), TEXT("whole.cvf"));
		FCFDVizVolumeReader Reader;
		TestTrue(TEXT("the untruncated file opens"), Reader.Open(Whole).IsOk());
	}

	// --- payloadOffset past the end, which no read would ever touch ----------
	//
	// payloadOffset is informational - each entry carries its own absolute
	// offset and that is what gets bounds-checked - but a value outside the
	// file means the header is internally inconsistent, and the rest of it
	// should not be trusted on the strength of "we never read that field".
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetPayloadOffset, 100000);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("beyond.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("payloadOffset past the end of the file is refused"), Result.IsOk());
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);
		TestEqual(TEXT("...naming byte 80"), Result.ByteOffset, static_cast<int64>(HdrOffsetPayloadOffset));
	}
	{
		// ...and inside the header, which is a different failure with the same
		// offset: the payload cannot start before the container does.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetPayloadOffset, 127);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("overlap.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("payloadOffset inside the 128-byte header is refused"), Result.IsOk());
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);

		// POSITIVE CONTROL: 128 is legal, and so is FileSize - a fully sparse
		// volume has no payload bytes at all, so payloadOffset == size is a
		// real file, not a truncated one. A bounds check written with the wrong
		// comparison would reject one of these.
		TArray<uint8> AtHeaderEnd = CopyOfGolden();
		WriteCvfUInt64At(AtHeaderEnd, HdrOffsetPayloadOffset, HdrBytes);
		ResealCvfHeaderCrc(AtHeaderEnd);
		const FCFDVizMemoryByteSource AtStart(AtHeaderEnd, TEXT("at-header-end.cvf"));
		FCFDVizVolumeReader StartReader;
		TestTrue(TEXT("payloadOffset == 128 is legal"), StartReader.Open(AtStart).IsOk());

		TArray<uint8> AtEnd = CopyOfGolden();
		WriteCvfUInt64At(AtEnd, HdrOffsetPayloadOffset, AtEnd.Num());
		ResealCvfHeaderCrc(AtEnd);
		const FCFDVizMemoryByteSource AtEndSource(AtEnd, TEXT("at-end.cvf"));
		FCFDVizVolumeReader EndReader;
		TestTrue(TEXT("payloadOffset == fileSize is legal: a sparse volume has no payload"),
			EndReader.Open(AtEndSource).IsOk());
	}

	// --- directoryOffset inside the header -----------------------------------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetDirectoryOffset, 100);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("dir-in-header.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("directoryOffset inside the header is refused"), Result.IsOk());
		TestTrue(TEXT("...as DirectoryOutOfBounds"), Result.Error == ECFDVizError::DirectoryOutOfBounds);
		TestEqual(TEXT("...naming byte 72"), Result.ByteOffset, static_cast<int64>(HdrOffsetDirectoryOffset));
	}

	// --- a hostile directoryOffset near 2^63 ---------------------------------
	//
	// The bounds check must subtract rather than add: offset + length is
	// exactly the sum that wraps when a corrupt header claims a position near
	// the top of the range, and a wrapped comparison is a rubber stamp
	// (format rule 1.5).
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetDirectoryOffset, 0x7FFFFFFFFFFFFF00ULL);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("huge-dir.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a directoryOffset near 2^63 cannot wrap into bounds"), Result.IsOk());
		TestTrue(TEXT("...as DirectoryOutOfBounds"), Result.Error == ECFDVizError::DirectoryOutOfBounds);
	}
	{
		// The same for a brick payload offset, which is checked per entry.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, GoldenEntryField(3, CvfTestEntryOffsetPayload), 0x7FFFFFFFFFFFFF00ULL);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("huge-payload.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a brick payload offset near 2^63 cannot wrap into bounds"), Result.IsOk());
		TestTrue(TEXT("...as PayloadOutOfBounds"), Result.Error == ECFDVizError::PayloadOutOfBounds);
	}

	// --- a directory entry short of 80 bytes ---------------------------------
	{
		FCFDVizBrickEntry Entry;
		const FCFDVizResult Result = FCFDVizBrickEntry::Parse(
			TArrayView<const uint8>(GoldenCvfBytes + GoldenDirectoryOffset, CvfTestEntryBytes - 1), Entry, 128);
		TestFalse(TEXT("79 bytes is not a directory entry"), Result.IsOk());
		TestTrue(TEXT("...as FileTooSmall"), Result.Error == ECFDVizError::FileTooSmall);
		TestEqual(TEXT("...naming the entry's own file offset"), Result.ByteOffset, static_cast<int64>(128));

		// POSITIVE CONTROL at exactly 80.
		TestTrue(TEXT("exactly 80 bytes parses"),
			FCFDVizBrickEntry::Parse(
				TArrayView<const uint8>(GoldenCvfBytes + GoldenDirectoryOffset, CvfTestEntryBytes), Entry, 128).IsOk());
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Edge bricks are clipped, never padded (section 4.4.3)                        */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderEdgeBrickTest,
	"FlowViz.CFDViz.VolumeReader.EdgeBricks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderEdgeBrickTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	// THIS TEST IS ONLY MEANINGFUL BECAUSE OF THE GEOMETRY. Value extent
	// (5, 3, 6) over bricks of (3, 2, 4): 5 = 3 + 2, 3 = 2 + 1, 6 = 4 + 2. No
	// axis divides evenly, so every axis has a partial edge brick, and the
	// three remainders (2, 1, 2) are all different from each other and from the
	// brick sizes. With (8,8,8) over brick size 8 there is one brick, the
	// remainder equals the brick size, and a reader that ignored clipping
	// entirely would pass.

	FCFDVizVolumeHeader Header;
	if (!TestTrue(TEXT("the golden header parses"),
		FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(GoldenCvfBytes, HdrBytes), Header).IsOk()))
	{
		return false;
	}

	// --- the interior brick gets the FULL size -------------------------------
	//
	// Paired with the clipped cases below: a reader that clipped everything
	// would fail here, and one that clipped nothing would fail there. Neither
	// assertion alone can distinguish the two.
	TestEqual(TEXT("brick (0,0,0) is interior on every axis and keeps the full (3,2,4)"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 0)), FIntVector(3, 2, 4));

	// --- one axis clipped at a time ------------------------------------------
	//
	// Each of these isolates a single axis, so a reader that applied the X
	// remainder to Y - or that computed the remainder from Dimensions rather
	// than from the value extent - lands on a different vector than the one
	// asserted.
	TestEqual(TEXT("brick (1,0,0) is clipped on X only: 5-3 = 2"),
		Header.GetValidSizeForBrick(FIntVector(1, 0, 0)), FIntVector(2, 2, 4));
	TestEqual(TEXT("brick (0,1,0) is clipped on Y only: 3-2 = 1"),
		Header.GetValidSizeForBrick(FIntVector(0, 1, 0)), FIntVector(3, 1, 4));
	TestEqual(TEXT("brick (0,0,1) is clipped on Z only: 6-4 = 2"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 1)), FIntVector(3, 2, 2));

	// --- two axes, and then all three ----------------------------------------
	TestEqual(TEXT("brick (1,1,0) is clipped on X and Y"),
		Header.GetValidSizeForBrick(FIntVector(1, 1, 0)), FIntVector(2, 1, 4));
	TestEqual(TEXT("brick (1,0,1) is clipped on X and Z"),
		Header.GetValidSizeForBrick(FIntVector(1, 0, 1)), FIntVector(2, 2, 2));
	TestEqual(TEXT("brick (0,1,1) is clipped on Y and Z"),
		Header.GetValidSizeForBrick(FIntVector(0, 1, 1)), FIntVector(3, 1, 2));
	TestEqual(TEXT("brick (1,1,1) is clipped on all three"),
		Header.GetValidSizeForBrick(FIntVector(1, 1, 1)), FIntVector(2, 1, 2));

	// --- the clipped sizes sum back to the extent ----------------------------
	//
	// A bug that returned brickSize for edge bricks would make these sums 6, 4
	// and 8 instead of 5, 3 and 6 - the exact overrun that shears a volume by a
	// row and still renders as plausible turbulence.
	TestEqual(TEXT("the X sizes tile the 5-wide extent exactly"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 0)).X + Header.GetValidSizeForBrick(FIntVector(1, 0, 0)).X, 5);
	TestEqual(TEXT("the Y sizes tile the 3-deep extent exactly"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 0)).Y + Header.GetValidSizeForBrick(FIntVector(0, 1, 0)).Y, 3);
	TestEqual(TEXT("the Z sizes tile the 6-tall extent exactly"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 0)).Z + Header.GetValidSizeForBrick(FIntVector(0, 0, 1)).Z, 6);

	// --- outside the tiling is (0,0,0), which no size can be mistaken for ----
	TestEqual(TEXT("a brick past the X grid has no size"),
		Header.GetValidSizeForBrick(FIntVector(2, 0, 0)), FIntVector(0, 0, 0));
	TestEqual(TEXT("a brick past the Y grid has no size"),
		Header.GetValidSizeForBrick(FIntVector(0, 2, 0)), FIntVector(0, 0, 0));
	TestEqual(TEXT("a brick past the Z grid has no size"),
		Header.GetValidSizeForBrick(FIntVector(0, 0, 2)), FIntVector(0, 0, 0));
	TestEqual(TEXT("a negative brick coordinate has no size"),
		Header.GetValidSizeForBrick(FIntVector(-1, 0, 0)), FIntVector(0, 0, 0));

	// --- ContainsValue draws the same boundary -------------------------------
	TestTrue(TEXT("(4,2,5) is the last value in the extent"), Header.ContainsValue(4, 2, 5));
	TestFalse(TEXT("(5,2,5) is one past on X"), Header.ContainsValue(5, 2, 5));
	TestFalse(TEXT("(4,3,5) is one past on Y"), Header.ContainsValue(4, 3, 5));
	TestFalse(TEXT("(4,2,6) is one past on Z"), Header.ContainsValue(4, 2, 6));
	TestFalse(TEXT("a negative index is outside"), Header.ContainsValue(-1, 0, 0));

	// --- and the reader REFUSES an entry that declares a padded edge brick ---
	//
	// This is the check that stops the shear before it can happen: an entry
	// claiming a full 3x2x4 cube at brick (1,1,1) is rejected at Open, with the
	// size the tiling requires named in the message.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, GoldenEntryField(7, CvfTestEntryOffsetValidSize), 3);	// validSizeX 2 -> 3
		// uncompressedBytes is made consistent with the LIE (3*1*2*2 = 12) so
		// that the entry is internally coherent and the ONLY thing wrong with it
		// is its relationship to the tiling. A reader that had no tiling check
		// but did have the section 4.4.4 size equality would accept this file.
		WriteCvfUInt32At(Bytes, GoldenEntryField(7, CvfTestEntryOffsetUncompressed), 12);
		WriteCvfUInt32At(Bytes, GoldenEntryField(7, CvfTestEntryOffsetCompressed), 12);
		ResealBrickCrc(Bytes, 7);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("padded-edge.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("an edge brick claiming a full cube is refused"), Result.IsOk());
		TestTrue(TEXT("...as SizeMismatch"), Result.Error == ECFDVizError::SizeMismatch);
		TestEqual(TEXT("...naming that entry's validSize field"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(7, CvfTestEntryOffsetValidSize)));
	}
	{
		// The mirror image: an INTERIOR brick that under-declares. A reader that
		// only checked "validSize <= brickSize" would accept this and then
		// reconstruct the volume with a hole in it.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetValidSize + 4), 3);	// validSizeZ 4 -> 3
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetUncompressed), 36);	// 3*2*3*2
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetCompressed), 36);
		ResealBrickCrc(Bytes, 0);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("short-interior.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("an interior brick that under-declares its extent is refused"), Result.IsOk());
		TestTrue(TEXT("...as SizeMismatch"), Result.Error == ECFDVizError::SizeMismatch);
	}
	{
		// A zero on any axis is refused by the same equality: the expected
		// remainder is always at least 1, so a zero can never satisfy it.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetValidSize), 0);
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetUncompressed), 0);
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetCompressed), 0);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("zero-extent.cvf"));
		FCFDVizVolumeReader Reader;
		TestFalse(TEXT("a brick with a zero extent is refused"), Reader.Open(Source).IsOk());
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Directory / header consistency                                               */
/* -------------------------------------------------------------------------- */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCFDVizVolumeReaderConsistencyTest,
	"FlowViz.CFDViz.VolumeReader.Consistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCFDVizVolumeReaderConsistencyTest::RunTest(const FString& Parameters)
{
	using namespace CvfReaderTest;

	// --- brickCount past what the tiling can hold ----------------------------
	//
	// A 2x2x2 grid holds at most 8 bricks. 9 is one too many - the off-by-one a
	// `>` written as `>=` would let through, and the value that then reads an
	// 81st entry's worth of bytes past the directory.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetBrickCount, 9);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("too-many.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("brickCount 9 exceeds the 8 a 2x2x2 grid can hold"), Result.IsOk());
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);
		TestEqual(TEXT("...naming brickCount @64"), Result.ByteOffset, static_cast<int64>(HdrOffsetBrickCount));
	}

	// --- dimensions that disagree with the directory -------------------------
	//
	// The header is edited, not the directory: shrinking the volume to (3,3,6)
	// makes the brick grid 1x2x2, so the four entries at brickIndex.X == 1 name
	// bricks that no longer exist. This is the "BrickCount disagrees with
	// Dimensions/BrickSize" case coming from the other direction, and it is the
	// one that matters, because a truncated grid with a full directory is what
	// a corrupted dimension field actually produces.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetDimensions, 3);	// dimensionX 5 -> 3
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("shrunk.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a dimension that shrinks the grid below the directory is refused"), Result.IsOk());
		// 1*2*2 = 4 bricks in the new tiling, but brickCount still says 8, so
		// the count check fires before any entry is read.
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);
		TestEqual(TEXT("...naming brickCount @64"), Result.ByteOffset, static_cast<int64>(HdrOffsetBrickCount));
	}
	{
		// The same shrink with brickCount lowered to match, so the count check
		// passes and the per-entry tiling check is what has to catch it. The
		// entries at brickIndex.X == 1 are now outside a 1x2x2 grid.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetDimensions, 3);
		WriteCvfUInt64At(Bytes, HdrOffsetBrickCount, 4);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("shrunk-consistent.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("an entry outside the tiling is refused"), Result.IsOk());
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);
		// Entry 1 is brick (1,0,0), the first one outside a 1-wide grid.
		TestEqual(TEXT("...naming entry 1's brickIndex"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(1, CvfTestEntryOffsetBrickIndex)));
	}

	// --- a brickSize that changes the tiling ---------------------------------
	//
	// brickSizeX 3 -> 5 makes the grid 1x2x2 with a single full-width brick per
	// row. Every entry's validSize is now wrong for its position even though
	// nothing in the directory changed.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, HdrOffsetBrickSize, 5);
		WriteCvfUInt64At(Bytes, HdrOffsetBrickCount, 4);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("wrong-bricksize.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a brickSize that changes the tiling invalidates the directory"), Result.IsOk());
		// Entry 0 is brick (0,0,0) and now must be 5 wide, not 3.
		TestTrue(TEXT("...as SizeMismatch"), Result.Error == ECFDVizError::SizeMismatch);
		TestEqual(TEXT("...naming entry 0's validSize"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(0, CvfTestEntryOffsetValidSize)));
	}

	// --- brickCount BELOW the tiling is legal (section 4.4.5) ---------------
	//
	// THE POSITIVE CONTROL for the two rejections above. Omitting bricks is how
	// empty space costs nothing, so a reader that enforced equality rather than
	// an upper bound would refuse every sparse file ever written. Lopping the
	// last four entries off leaves a legal file whose absent bricks read back
	// as backgroundValue.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetBrickCount, 4);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("half.cvf"));
		FCFDVizVolumeReader Reader;
		if (TestTrue(TEXT("a directory with fewer bricks than the tiling is legal"), Reader.Open(Source).IsOk()))
		{
			TestEqual(TEXT("only the declared entries are loaded"), Reader.GetBrickCount(), 4);
			TestEqual(TEXT("the tiling still describes 8"),
				Reader.GetHeader().GetTotalBrickCount(), static_cast<int64>(8));
			TestEqual(TEXT("an omitted brick is not in the lookup"),
				Reader.FindBrickByCoordinate(FIntVector(0, 0, 1)), INDEX_NONE);

			// backgroundValue is 0 in this fixture, so an omitted brick reads
			// back as zeros - and the PRESENT bricks must still carry their real
			// values, which is what distinguishes "filled with background" from
			// "the whole read returned zeros".
			TArray<uint8> Voxel;
			if (TestTrue(TEXT("a voxel in an omitted brick reads"), Reader.ReadVoxel(0, 0, 4, Voxel).IsOk()))
			{
				TestEqual(TEXT("...as backgroundValue component 0"), static_cast<int32>(Voxel[0]), 0);
				TestEqual(TEXT("...and backgroundValue component 1"), static_cast<int32>(Voxel[1]), 0);
			}
			if (TestTrue(TEXT("a voxel in a present brick still reads"), Reader.ReadVoxel(2, 1, 3, Voxel).IsOk()))
			{
				TestEqual(TEXT("...its real component 0"),
					static_cast<int32>(Voxel[0]), static_cast<int32>(GoldenValueAt(2, 1, 3, 0)));
				TestEqual(TEXT("...and its real component 1"),
					static_cast<int32>(Voxel[1]), static_cast<int32>(GoldenValueAt(2, 1, 3, 1)));
			}
		}
	}

	// --- brickCount 0 is legal too -------------------------------------------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt64At(Bytes, HdrOffsetBrickCount, 0);
		ResealCvfHeaderCrc(Bytes);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("empty.cvf"));
		FCFDVizVolumeReader Reader;
		if (TestTrue(TEXT("a volume with no bricks at all opens"), Reader.Open(Source).IsOk()))
		{
			TestEqual(TEXT("the directory is empty"), Reader.GetBrickCount(), 0);
			TArray<uint8> Dense;
			if (TestTrue(TEXT("ReadDense still returns a full volume"), Reader.ReadDense(Dense).IsOk()))
			{
				TestEqual(TEXT("...of the declared size"), Dense.Num(), 180);
				bool bAllBackground = true;
				for (uint8 Byte : Dense)
				{
					bAllBackground = bAllBackground && Byte == 0;
				}
				TestTrue(TEXT("...entirely backgroundValue"), bAllBackground);
			}
		}
	}

	// --- duplicate directory entries -----------------------------------------
	//
	// Two entries for one brick leaves which copy wins undefined, and worse,
	// resolved differently by FindBrickByCoordinate (first) and ReadDense
	// (last written) - so ReadVoxel and ReadDense would disagree about the same
	// file. Entry 1's brickIndex is rewritten to (0,0,0), which entry 0 already
	// claims.
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, GoldenEntryField(1, CvfTestEntryOffsetBrickIndex), 0);	// (1,0,0) -> (0,0,0)
		WriteUInt16At(Bytes, GoldenEntryField(1, CvfTestEntryOffsetValidSize), 3);	// keep validSize legal for (0,0,0)
		WriteCvfUInt32At(Bytes, GoldenEntryField(1, CvfTestEntryOffsetUncompressed), 48);
		WriteCvfUInt32At(Bytes, GoldenEntryField(1, CvfTestEntryOffsetCompressed), 48);
		ResealBrickCrc(Bytes, 1);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("duplicate.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("two entries for one brick coordinate are refused"), Result.IsOk());
		TestTrue(TEXT("...as InvalidHeader"), Result.Error == ECFDVizError::InvalidHeader);
		TestEqual(TEXT("...naming the SECOND entry, the one that collided"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(1, CvfTestEntryOffsetBrickIndex)));
	}

	// --- the section 4.4.4 size equality, checked BEFORE any allocation ------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetUncompressed), 49);	// 48 + 1

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("size-lie.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("uncompressedBytes one byte off the derived size is refused"), Result.IsOk());
		TestTrue(TEXT("...as SizeMismatch"), Result.Error == ECFDVizError::SizeMismatch);
		TestEqual(TEXT("...naming uncompressedBytes @32 of that entry"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(0, CvfTestEntryOffsetUncompressed)));
	}
	{
		// A hostile value, to prove the check is not a soft plausibility test:
		// 4 GB declared for a 24-voxel brick.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetUncompressed), 0xFFFFFFFFu);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("huge-size.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("a 4 GB uncompressedBytes on a 48-byte brick is refused"), Result.IsOk());
		TestTrue(TEXT("...as SizeMismatch, before anything is allocated"),
			Result.Error == ECFDVizError::SizeMismatch);
	}
	{
		// codec none means the stored and decoded lengths are the same number
		// by definition. A file that says otherwise describes a transformation
		// it did not perform.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetCompressed), 40);
		ResealBrickCrc(Bytes, 0);

		const FCFDVizMemoryByteSource Source(Bytes, TEXT("codec-none-mismatch.cvf"));
		FCFDVizVolumeReader Reader;
		const FCFDVizResult Result = Reader.Open(Source);
		TestFalse(TEXT("codec none with compressed != uncompressed is refused"), Result.IsOk());
		TestTrue(TEXT("...as SizeMismatch"), Result.Error == ECFDVizError::SizeMismatch);
		TestEqual(TEXT("...naming compressedBytes @28"),
			Result.ByteOffset, static_cast<int64>(GoldenEntryField(0, CvfTestEntryOffsetCompressed)));
	}

	// --- reserved fields the spec pins to zero -------------------------------
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrOffsetReservedShort] = 1;
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		TestFalse(TEXT("reserved bytes [62,64) must be zero"), Result.IsOk());
		TestEqual(TEXT("...naming byte 62"), Result.ByteOffset, static_cast<int64>(HdrOffsetReservedShort));
	}
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[HdrBytes - 1] = 0x80;	// the last byte of the reserved tail
		ResealCvfHeaderCrc(Bytes);

		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		TestFalse(TEXT("reserved bytes [108,128) must be zero, to the last byte"), Result.IsOk());
		TestEqual(TEXT("...naming byte 108"), Result.ByteOffset, static_cast<int64>(HdrOffsetReservedTail));
	}
	{
		TArray<uint8> Bytes = CopyOfGolden();
		Bytes[GoldenEntryField(0, CvfTestEntryOffsetReserved) + 7] = 1;

		FCFDVizBrickEntry Entry;
		const FCFDVizResult Result = FCFDVizBrickEntry::Parse(
			TArrayView<const uint8>(Bytes.GetData() + GoldenDirectoryOffset, CvfTestEntryBytes), Entry, 128);
		TestFalse(TEXT("reserved entry bytes [72,80) must be zero"), Result.IsOk());
		TestEqual(TEXT("...naming the entry's byte 72"), Result.ByteOffset, static_cast<int64>(128 + CvfTestEntryOffsetReserved));
	}
	{
		// No per-brick flag bits exist in 1.0. An unknown one could change how
		// the payload decodes, so it is refused rather than ignored.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteUInt16At(Bytes, GoldenEntryField(0, CvfTestEntryOffsetEntryFlags), 0x0001);

		FCFDVizBrickEntry Entry;
		const FCFDVizResult Result = FCFDVizBrickEntry::Parse(
			TArrayView<const uint8>(Bytes.GetData() + GoldenDirectoryOffset, CvfTestEntryBytes), Entry, 128);
		TestFalse(TEXT("an unknown per-brick flag bit is refused, not ignored"), Result.IsOk());
		TestEqual(TEXT("...naming the entry's byte 18"), Result.ByteOffset, static_cast<int64>(128 + CvfTestEntryOffsetEntryFlags));
	}

	// --- header field validation ---------------------------------------------
	//
	// The enum bytes, the magic, the endian marker, the version policy and the
	// degenerate-geometry cases live in CFDVizVolumeIntegrityTest.cpp, which owns
	// the rejection catalogue. What follows is deliberately the part that file
	// does not cover: the flag bits, the componentCount POSITIVE controls, and
	// the reader's refusal to fabricate an extent it cannot address.
	{
		// An unknown header flag bit could change how payloads decode, so
		// reading on "as if it were clear" would silently produce wrong data.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetFlags, 0x00000002);
		ResealCvfHeaderCrc(Bytes);
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		TestFalse(TEXT("an unknown header flag bit is refused, not ignored"), Result.IsOk());
		TestEqual(TEXT("...naming byte 20"), Result.ByteOffset, static_cast<int64>(HdrOffsetFlags));

		// POSITIVE CONTROL: bit 0 IS known, and must not be refused.
		TArray<uint8> Sparse = CopyOfGolden();
		WriteCvfUInt32At(Sparse, HdrOffsetFlags, 0x00000001);
		ResealCvfHeaderCrc(Sparse);
		FCFDVizVolumeHeader SparseHeader;
		if (TestTrue(TEXT("the sparse flag bit is accepted"),
			FCFDVizVolumeHeader::Parse(
				TArrayView<const uint8>(Sparse.GetData(), HdrBytes), SparseHeader).IsOk()))
		{
			TestTrue(TEXT("...and IsSparse reports it"), SparseHeader.IsSparse());
		}
	}
	{
		// componentCount is capped at 4 by the float32[4] background and
		// statistics arrays, and 0 is meaningless.
		for (uint8 Count : { static_cast<uint8>(0), static_cast<uint8>(5), static_cast<uint8>(255) })
		{
			TArray<uint8> Bytes = CopyOfGolden();
			Bytes[HdrOffsetComponentCount] = Count;
			ResealCvfHeaderCrc(Bytes);
			FCFDVizVolumeHeader Header;
			const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
				TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
			TestFalse(*FString::Printf(TEXT("componentCount %u is outside 1..4"), Count), Result.IsOk());
			TestEqual(TEXT("...naming byte 58"), Result.ByteOffset, static_cast<int64>(HdrOffsetComponentCount));
		}
		// POSITIVE CONTROL at each end of the legal range.
		for (uint8 Count : { static_cast<uint8>(1), static_cast<uint8>(4) })
		{
			TArray<uint8> Bytes = CopyOfGolden();
			Bytes[HdrOffsetComponentCount] = Count;
			ResealCvfHeaderCrc(Bytes);
			FCFDVizVolumeHeader Header;
			TestTrue(*FString::Printf(TEXT("componentCount %u is legal"), Count),
				FCFDVizVolumeHeader::Parse(TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header).IsOk());
		}
	}
	{
		// A zero on the Y axis specifically. The integrity suite proves a zero
		// dimensionX is refused; this proves the check is not written against
		// axis X alone, and that the reported offset is the START of the
		// dimensions triple rather than the individual axis that was zero.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetDimensions + 4, 0);
		ResealCvfHeaderCrc(Bytes);
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		TestFalse(TEXT("a zero dimensionY is refused, not just a zero dimensionX"), Result.IsOk());
		TestEqual(TEXT("...naming byte 40"), Result.ByteOffset, static_cast<int64>(HdrOffsetDimensions));

		// Likewise brickSizeY, not brickSizeX.
		TArray<uint8> ZeroBrick = CopyOfGolden();
		WriteUInt16At(ZeroBrick, HdrOffsetBrickSize + 2, 0);
		ResealCvfHeaderCrc(ZeroBrick);
		const FCFDVizResult BrickResult = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(ZeroBrick.GetData(), HdrBytes), Header);
		TestFalse(TEXT("a zero brickSizeY is refused"), BrickResult.IsOk());
		TestEqual(TEXT("...naming byte 52"), BrickResult.ByteOffset, static_cast<int64>(HdrOffsetBrickSize));
	}
	{
		// A dimension past what this build can index. The file may be legal;
		// saying so is more honest than overflowing an extent and then
		// bounds-checking against the wrapped value.
		TArray<uint8> Bytes = CopyOfGolden();
		WriteCvfUInt32At(Bytes, HdrOffsetDimensions, 0xFFFFFFFFu);
		ResealCvfHeaderCrc(Bytes);
		FCFDVizVolumeHeader Header;
		const FCFDVizResult Result = FCFDVizVolumeHeader::Parse(
			TArrayView<const uint8>(Bytes.GetData(), HdrBytes), Header);
		TestFalse(TEXT("a dimension past MaxGridDimension is refused"), Result.IsOk());
		TestTrue(TEXT("...as AllocationTooLarge, not as corruption"),
			Result.Error == ECFDVizError::AllocationTooLarge);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
