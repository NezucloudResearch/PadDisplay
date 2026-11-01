// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
#include "capture/cursor_overlay.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace pd;

namespace
{

    int failures = 0;

    constexpr uint32_t kTransparent = 0, kOpaqueBlack = 0xFF000000u, kOpaqueWhite = 0xFFFFFFFFu;

    void Expect(const char *test, bool passed)
    {
        if (passed)
            return;
        std::printf("FAILED %s\n", test);
        ++failures;
    }

    DXGI_OUTDUPL_POINTER_SHAPE_INFO Info(UINT type, UINT width, UINT height, UINT pitch)
    {
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
        info.Type = type;
        info.Width = width;
        info.Height = height;
        info.Pitch = pitch;
        return info;
    }

    void PutPixel(std::vector<uint8_t> &shape, size_t offset, uint32_t pixel)
    {
        std::memcpy(&shape[offset], &pixel, sizeof(pixel));
    }

    void AColorShapeIsCopiedAcrossItsPitch()
    {
        constexpr UINT kPitch = 12; // wider than the two pixels of a row
        std::vector<uint8_t> shape(size_t(kPitch) * 2, 0xEE);
        PutPixel(shape, 0, 0x11223344);
        PutPixel(shape, 4, 0x55667788);
        PutPixel(shape, kPitch, 0x99AABBCC);
        PutPixel(shape, kPitch + 4, 0xDDEEFF00);
        const CursorImage image = DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 2, kPitch), shape);
        Expect("size", image.width == 2 && image.height == 2);
        Expect("pixels", image.color == std::vector<uint32_t>{0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00});
        Expect("no invert layer", !image.hasInvert);
    }

    // One row of four pixels, one for each combination of the AND and XOR bits.
    void MonochromeHasFourOutcomes()
    {
        const std::vector<uint8_t> shape{0b00110000, 0b01010000}; // AND row, then XOR row
        const CursorImage image = DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 4, 2, 1), shape);
        Expect("the height holds both masks", image.height == 1);
        Expect("and 0, xor 0 is black", image.color[0] == kOpaqueBlack);
        Expect("and 0, xor 1 is white", image.color[1] == kOpaqueWhite);
        Expect("and 1, xor 0 is transparent", image.color[2] == kTransparent && image.invert[2] == kTransparent);
        Expect("and 1, xor 1 inverts", image.color[3] == kTransparent && image.invert[3] == kOpaqueWhite);
        Expect("the invert layer is noted", image.hasInvert);
    }

    void MaskedColorHasThreeOutcomes()
    {
        std::vector<uint8_t> shape(12);
        PutPixel(shape, 0, 0x00336699); // mask 0: replaces the screen
        PutPixel(shape, 4, 0xFF336699); // mask set, colour: inverts
        PutPixel(shape, 8, 0xFF000000); // mask set, black: leaves the screen alone
        const CursorImage image = DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 3, 1, 12), shape);
        Expect("replaces, made opaque", image.color[0] == 0xFF336699);
        Expect("inverts", image.invert[1] == 0xFF336699 && image.color[1] == kTransparent);
        Expect("leaves alone", image.color[2] == kTransparent && image.invert[2] == kTransparent);
        Expect("the invert layer is noted", image.hasInvert);
    }

    void ReplacingPixelsNeedNoInvertLayer()
    {
        std::vector<uint8_t> shape(4);
        PutPixel(shape, 0, 0x00336699);
        Expect("no invert", !DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 1, 1, 4), shape).hasInvert);
    }

    void AShapeWithNoPixelsGivesNoImage()
    {
        Expect("no width", DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 0, 2, 0), {}).width == 0);
        Expect("no height", DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 2, 0, 8), {}).width == 0);
        Expect("one monochrome row is only half a mask", DecodePointerShape(Info(DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 2, 1, 1), {0}).width == 0);
    }

} // namespace

int main()
{
    AColorShapeIsCopiedAcrossItsPitch();
    MonochromeHasFourOutcomes();
    MaskedColorHasThreeOutcomes();
    ReplacingPixelsNeedNoInvertLayer();
    AShapeWithNoPixelsGivesNoImage();
    if (failures == 0)
        std::puts("cursorshape: all tests passed");
    return failures == 0 ? 0 : 1;
}
