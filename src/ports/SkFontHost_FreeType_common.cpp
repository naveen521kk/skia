/*
 * Copyright 2006-2012 The Android Open Source Project
 * Copyright 2012 Mozilla Foundation
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkDrawable.h"
#include "include/core/SkPath.h"
#include "include/effects/SkGradientShader.h"
#include "include/pathops/SkPathOps.h"
#include "include/private/SkColorData.h"
#include "include/private/SkTo.h"
#include "src/core/SkFDot6.h"
#include "src/ports/SkFontHost_FreeType_common.h"

#include <algorithm>
#include <utility>

#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/ftbitmap.h>
#ifdef FT_COLOR_H
#   include <freetype/ftcolor.h>
#endif
#include <freetype/ftimage.h>
#include <freetype/ftoutln.h>
#include <freetype/ftsizes.h>
// In the past, FT_GlyphSlot_Own_Bitmap was defined in this header file.
#include <freetype/ftsynth.h>

namespace {
[[maybe_unused]] static inline const constexpr bool kSkShowTextBlitCoverage = false;
}

#ifdef TT_SUPPORT_COLRV1
// FT_ClipBox and FT_Get_Color_Glyph_ClipBox introduced VER-2-11-0-18-g47cf8ebf4
// FT_COLR_COMPOSITE_PLUS and renumbering introduced VER-2-11-0-21-ge40ae7569
// FT_SIZEOF_LONG_LONG introduced VER-2-11-0-31-gffdac8d67
// FT_PaintRadialGradient changed size and layout at VER-2-11-0-147-gd3d3ff76d
// FT_STATIC_CAST introduced VER-2-11-0-172-g9079c5d91
// So undefine TT_SUPPORT_COLRV1 before 2.11.1 but not if FT_STATIC_CAST is defined.
#if (((FREETYPE_MAJOR)  < 2) || \
     ((FREETYPE_MAJOR) == 2 && (FREETYPE_MINOR)  < 11) || \
     ((FREETYPE_MAJOR) == 2 && (FREETYPE_MINOR) == 11 && (FREETYPE_PATCH) < 1)) && \
    !defined(FT_STATIC_CAST)
#    undef TT_SUPPORT_COLRV1
#else
#    include "src/core/SkScopeExit.h"
#endif
#endif

// FT_OUTLINE_OVERLAP was added in FreeType 2.10.3
#ifndef FT_OUTLINE_OVERLAP
#    define FT_OUTLINE_OVERLAP 0x40
#endif

// FT_LOAD_COLOR and the corresponding FT_Pixel_Mode::FT_PIXEL_MODE_BGRA
// were introduced in FreeType 2.5.0.
// The following may be removed once FreeType 2.5.0 is required to build.
#ifndef FT_LOAD_COLOR
#    define FT_LOAD_COLOR ( 1L << 20 )
#    define FT_PIXEL_MODE_BGRA 7
#endif

#ifdef SK_DEBUG
const char* SkTraceFtrGetError(int e) {
    switch ((FT_Error)e) {
        #undef FTERRORS_H_
        #define FT_ERRORDEF( e, v, s ) case v: return s;
        #define FT_ERROR_START_LIST
        #define FT_ERROR_END_LIST
        #include FT_ERRORS_H
        #undef FT_ERRORDEF
        #undef FT_ERROR_START_LIST
        #undef FT_ERROR_END_LIST
        default: return "";
    }
}
#endif  // SK_DEBUG

#ifdef TT_SUPPORT_COLRV1
bool operator==(const FT_OpaquePaint& a, const FT_OpaquePaint& b) {
    return a.p == b.p && a.insert_root_transform == b.insert_root_transform;
}
#endif

namespace {

FT_Pixel_Mode compute_pixel_mode(SkMask::Format format) {
    switch (format) {
        case SkMask::kBW_Format:
            return FT_PIXEL_MODE_MONO;
        case SkMask::kA8_Format:
        default:
            return FT_PIXEL_MODE_GRAY;
    }
}

///////////////////////////////////////////////////////////////////////////////

uint16_t packTriple(U8CPU r, U8CPU g, U8CPU b) {
    if constexpr (kSkShowTextBlitCoverage) {
        r = std::max(r, (U8CPU)0x40);
        g = std::max(g, (U8CPU)0x40);
        b = std::max(b, (U8CPU)0x40);
    }
    return SkPack888ToRGB16(r, g, b);
}

uint16_t grayToRGB16(U8CPU gray) {
    if constexpr (kSkShowTextBlitCoverage) {
        gray = std::max(gray, (U8CPU)0x40);
    }
    return SkPack888ToRGB16(gray, gray, gray);
}

int bittst(const uint8_t data[], int bitOffset) {
    SkASSERT(bitOffset >= 0);
    int lowBit = data[bitOffset >> 3] >> (~bitOffset & 7);
    return lowBit & 1;
}

/**
 *  Copies a FT_Bitmap into an SkMask with the same dimensions.
 *
 *  FT_PIXEL_MODE_MONO
 *  FT_PIXEL_MODE_GRAY
 *  FT_PIXEL_MODE_LCD
 *  FT_PIXEL_MODE_LCD_V
 */
template<bool APPLY_PREBLEND>
void copyFT2LCD16(const FT_Bitmap& bitmap, const SkMask& mask, int lcdIsBGR,
                  const uint8_t* tableR, const uint8_t* tableG, const uint8_t* tableB)
{
    SkASSERT(SkMask::kLCD16_Format == mask.fFormat);
    if (FT_PIXEL_MODE_LCD != bitmap.pixel_mode) {
        SkASSERT(mask.fBounds.width() == static_cast<int>(bitmap.width));
    }
    if (FT_PIXEL_MODE_LCD_V != bitmap.pixel_mode) {
        SkASSERT(mask.fBounds.height() == static_cast<int>(bitmap.rows));
    }

    const uint8_t* src = bitmap.buffer;
    uint16_t* dst = reinterpret_cast<uint16_t*>(mask.fImage);
    const size_t dstRB = mask.fRowBytes;

    const int width = mask.fBounds.width();
    const int height = mask.fBounds.height();

    switch (bitmap.pixel_mode) {
        case FT_PIXEL_MODE_MONO:
            for (int y = height; y --> 0;) {
                for (int x = 0; x < width; ++x) {
                    dst[x] = -bittst(src, x);
                }
                dst = (uint16_t*)((char*)dst + dstRB);
                src += bitmap.pitch;
            }
            break;
        case FT_PIXEL_MODE_GRAY:
            for (int y = height; y --> 0;) {
                for (int x = 0; x < width; ++x) {
                    dst[x] = grayToRGB16(src[x]);
                }
                dst = (uint16_t*)((char*)dst + dstRB);
                src += bitmap.pitch;
            }
            break;
        case FT_PIXEL_MODE_LCD:
            SkASSERT(3 * mask.fBounds.width() == static_cast<int>(bitmap.width));
            for (int y = height; y --> 0;) {
                const uint8_t* triple = src;
                if (lcdIsBGR) {
                    for (int x = 0; x < width; x++) {
                        dst[x] = packTriple(sk_apply_lut_if<APPLY_PREBLEND>(triple[2], tableR),
                                            sk_apply_lut_if<APPLY_PREBLEND>(triple[1], tableG),
                                            sk_apply_lut_if<APPLY_PREBLEND>(triple[0], tableB));
                        triple += 3;
                    }
                } else {
                    for (int x = 0; x < width; x++) {
                        dst[x] = packTriple(sk_apply_lut_if<APPLY_PREBLEND>(triple[0], tableR),
                                            sk_apply_lut_if<APPLY_PREBLEND>(triple[1], tableG),
                                            sk_apply_lut_if<APPLY_PREBLEND>(triple[2], tableB));
                        triple += 3;
                    }
                }
                src += bitmap.pitch;
                dst = (uint16_t*)((char*)dst + dstRB);
            }
            break;
        case FT_PIXEL_MODE_LCD_V:
            SkASSERT(3 * mask.fBounds.height() == static_cast<int>(bitmap.rows));
            for (int y = height; y --> 0;) {
                const uint8_t* srcR = src;
                const uint8_t* srcG = srcR + bitmap.pitch;
                const uint8_t* srcB = srcG + bitmap.pitch;
                if (lcdIsBGR) {
                    using std::swap;
                    swap(srcR, srcB);
                }
                for (int x = 0; x < width; x++) {
                    dst[x] = packTriple(sk_apply_lut_if<APPLY_PREBLEND>(*srcR++, tableR),
                                        sk_apply_lut_if<APPLY_PREBLEND>(*srcG++, tableG),
                                        sk_apply_lut_if<APPLY_PREBLEND>(*srcB++, tableB));
                }
                src += 3 * bitmap.pitch;
                dst = (uint16_t*)((char*)dst + dstRB);
            }
            break;
        default:
            SkDEBUGF("FT_Pixel_Mode %d", bitmap.pixel_mode);
            SkDEBUGFAIL("unsupported FT_Pixel_Mode for LCD16");
            break;
    }
}

/**
 *  Copies a FT_Bitmap into an SkMask with the same dimensions.
 *
 *  Yes, No, Never Requested, Never Produced
 *
 *                        kBW kA8 k3D kARGB32 kLCD16
 *  FT_PIXEL_MODE_MONO     Y   Y  NR     N       Y
 *  FT_PIXEL_MODE_GRAY     N   Y  NR     N       Y
 *  FT_PIXEL_MODE_GRAY2   NP  NP  NR    NP      NP
 *  FT_PIXEL_MODE_GRAY4   NP  NP  NR    NP      NP
 *  FT_PIXEL_MODE_LCD     NP  NP  NR    NP      NP
 *  FT_PIXEL_MODE_LCD_V   NP  NP  NR    NP      NP
 *  FT_PIXEL_MODE_BGRA     N   N  NR     Y       N
 *
 *  TODO: All of these N need to be Y or otherwise ruled out.
 */
void copyFTBitmap(const FT_Bitmap& srcFTBitmap, SkMask& dstMask) {
    SkASSERTF(dstMask.fBounds.width() == static_cast<int>(srcFTBitmap.width),
              "dstMask.fBounds.width() = %d\n"
              "static_cast<int>(srcFTBitmap.width) = %d",
              dstMask.fBounds.width(),
              static_cast<int>(srcFTBitmap.width)
    );
    SkASSERTF(dstMask.fBounds.height() == static_cast<int>(srcFTBitmap.rows),
              "dstMask.fBounds.height() = %d\n"
              "static_cast<int>(srcFTBitmap.rows) = %d",
              dstMask.fBounds.height(),
              static_cast<int>(srcFTBitmap.rows)
    );

    const uint8_t* src = reinterpret_cast<const uint8_t*>(srcFTBitmap.buffer);
    const FT_Pixel_Mode srcFormat = static_cast<FT_Pixel_Mode>(srcFTBitmap.pixel_mode);
    // FT_Bitmap::pitch is an int and allowed to be negative.
    const int srcPitch = srcFTBitmap.pitch;
    const size_t srcRowBytes = SkTAbs(srcPitch);

    uint8_t* dst = dstMask.fImage;
    const SkMask::Format dstFormat = static_cast<SkMask::Format>(dstMask.fFormat);
    const size_t dstRowBytes = dstMask.fRowBytes;

    const size_t width = srcFTBitmap.width;
    const size_t height = srcFTBitmap.rows;

    if (SkMask::kLCD16_Format == dstFormat) {
        copyFT2LCD16<false>(srcFTBitmap, dstMask, false, nullptr, nullptr, nullptr);
        return;
    }

    if ((FT_PIXEL_MODE_MONO == srcFormat && SkMask::kBW_Format == dstFormat) ||
        (FT_PIXEL_MODE_GRAY == srcFormat && SkMask::kA8_Format == dstFormat))
    {
        size_t commonRowBytes = std::min(srcRowBytes, dstRowBytes);
        for (size_t y = height; y --> 0;) {
            memcpy(dst, src, commonRowBytes);
            src += srcPitch;
            dst += dstRowBytes;
        }
    } else if (FT_PIXEL_MODE_MONO == srcFormat && SkMask::kA8_Format == dstFormat) {
        for (size_t y = height; y --> 0;) {
            uint8_t byte = 0;
            int bits = 0;
            const uint8_t* src_row = src;
            uint8_t* dst_row = dst;
            for (size_t x = width; x --> 0;) {
                if (0 == bits) {
                    byte = *src_row++;
                    bits = 8;
                }
                *dst_row++ = byte & 0x80 ? 0xff : 0x00;
                bits--;
                byte <<= 1;
            }
            src += srcPitch;
            dst += dstRowBytes;
        }
    } else if (FT_PIXEL_MODE_BGRA == srcFormat && SkMask::kARGB32_Format == dstFormat) {
        // FT_PIXEL_MODE_BGRA is pre-multiplied.
        for (size_t y = height; y --> 0;) {
            const uint8_t* src_row = src;
            SkPMColor* dst_row = reinterpret_cast<SkPMColor*>(dst);
            for (size_t x = 0; x < width; ++x) {
                uint8_t b = *src_row++;
                uint8_t g = *src_row++;
                uint8_t r = *src_row++;
                uint8_t a = *src_row++;
                *dst_row++ = SkPackARGB32(a, r, g, b);
                if constexpr (kSkShowTextBlitCoverage) {
                    *(dst_row-1) = SkFourByteInterp256(*(dst_row-1), SK_ColorWHITE, 0x40);
                }
            }
            src += srcPitch;
            dst += dstRowBytes;
        }
    } else {
        SkDEBUGF("FT_Pixel_Mode %d, SkMask::Format %d\n", srcFormat, dstFormat);
        SkDEBUGFAIL("unsupported combination of FT_Pixel_Mode and SkMask::Format");
    }
}

inline int convert_8_to_1(unsigned byte) {
    SkASSERT(byte <= 0xFF);
    // Arbitrary decision that making the cutoff at 1/4 instead of 1/2 in general looks better.
    return (byte >> 6) != 0;
}

uint8_t pack_8_to_1(const uint8_t alpha[8]) {
    unsigned bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits <<= 1;
        bits |= convert_8_to_1(alpha[i]);
    }
    return SkToU8(bits);
}

void packA8ToA1(const SkMask& mask, const uint8_t* src, size_t srcRB) {
    const int height = mask.fBounds.height();
    const int width = mask.fBounds.width();
    const int octs = width >> 3;
    const int leftOverBits = width & 7;

    uint8_t* dst = mask.fImage;
    const int dstPad = mask.fRowBytes - SkAlign8(width)/8;
    SkASSERT(dstPad >= 0);

    const int srcPad = srcRB - width;
    SkASSERT(srcPad >= 0);

    for (int y = 0; y < height; ++y) {
        for (int i = 0; i < octs; ++i) {
            *dst++ = pack_8_to_1(src);
            src += 8;
        }
        if (leftOverBits > 0) {
            unsigned bits = 0;
            int shift = 7;
            for (int i = 0; i < leftOverBits; ++i, --shift) {
                bits |= convert_8_to_1(*src++) << shift;
            }
            *dst++ = bits;
        }
        src += srcPad;
        dst += dstPad;
    }
}

inline SkMask::Format SkMaskFormat_for_SkColorType(SkColorType colorType) {
    switch (colorType) {
        case kAlpha_8_SkColorType:
            return SkMask::kA8_Format;
        case kN32_SkColorType:
            return SkMask::kARGB32_Format;
        default:
            SkDEBUGFAIL("unsupported SkBitmap::Config");
            return SkMask::kA8_Format;
    }
}

inline SkColorType SkColorType_for_FTPixelMode(FT_Pixel_Mode pixel_mode) {
    switch (pixel_mode) {
        case FT_PIXEL_MODE_MONO:
        case FT_PIXEL_MODE_GRAY:
            return kAlpha_8_SkColorType;
        case FT_PIXEL_MODE_BGRA:
            return kN32_SkColorType;
        default:
            SkDEBUGFAIL("unsupported FT_PIXEL_MODE");
            return kAlpha_8_SkColorType;
    }
}

inline SkColorType SkColorType_for_SkMaskFormat(SkMask::Format format) {
    switch (format) {
        case SkMask::kBW_Format:
        case SkMask::kA8_Format:
        case SkMask::kLCD16_Format:
            return kAlpha_8_SkColorType;
        case SkMask::kARGB32_Format:
            return kN32_SkColorType;
        default:
            SkDEBUGFAIL("unsupported destination SkBitmap::Config");
            return kAlpha_8_SkColorType;
    }
}

// Only build COLRv1 rendering code if FreeType is new enough to have COLRv1
// additions. FreeType defines a macro in the ftoption header to tell us whether
// it does support these features.
#ifdef TT_SUPPORT_COLRV1

const uint16_t kForegroundColorPaletteIndex = 0xFFFF;

struct OpaquePaintHasher {
  size_t operator()(const FT_OpaquePaint& opaquePaint) {
      return SkGoodHash()(opaquePaint.p) ^
             SkGoodHash()(opaquePaint.insert_root_transform);
  }
};

using VisitedSet = SkTHashSet<FT_OpaquePaint, OpaquePaintHasher>;

bool generateFacePathCOLRv1(FT_Face face, SkGlyphID glyphID, SkPath* path);

inline float SkColrV1AlphaToFloat(uint16_t alpha) { return (alpha / float(1 << 14)); }


inline SkTileMode ToSkTileMode(FT_PaintExtend extendMode) {
    switch (extendMode) {
        case FT_COLR_PAINT_EXTEND_REPEAT:
            return SkTileMode::kRepeat;
        case FT_COLR_PAINT_EXTEND_REFLECT:
            return SkTileMode::kMirror;
        default:
            return SkTileMode::kClamp;
    }
}

inline SkBlendMode ToSkBlendMode(FT_Composite_Mode compositeMode) {
    switch (compositeMode) {
        case FT_COLR_COMPOSITE_CLEAR:
            return SkBlendMode::kClear;
        case FT_COLR_COMPOSITE_SRC:
            return SkBlendMode::kSrc;
        case FT_COLR_COMPOSITE_DEST:
            return SkBlendMode::kDst;
        case FT_COLR_COMPOSITE_SRC_OVER:
            return SkBlendMode::kSrcOver;
        case FT_COLR_COMPOSITE_DEST_OVER:
            return SkBlendMode::kDstOver;
        case FT_COLR_COMPOSITE_SRC_IN:
            return SkBlendMode::kSrcIn;
        case FT_COLR_COMPOSITE_DEST_IN:
            return SkBlendMode::kDstIn;
        case FT_COLR_COMPOSITE_SRC_OUT:
            return SkBlendMode::kSrcOut;
        case FT_COLR_COMPOSITE_DEST_OUT:
            return SkBlendMode::kDstOut;
        case FT_COLR_COMPOSITE_SRC_ATOP:
            return SkBlendMode::kSrcATop;
        case FT_COLR_COMPOSITE_DEST_ATOP:
            return SkBlendMode::kDstATop;
        case FT_COLR_COMPOSITE_XOR:
            return SkBlendMode::kXor;
        case FT_COLR_COMPOSITE_PLUS:
            return SkBlendMode::kPlus;
        case FT_COLR_COMPOSITE_SCREEN:
            return SkBlendMode::kScreen;
        case FT_COLR_COMPOSITE_OVERLAY:
            return SkBlendMode::kOverlay;
        case FT_COLR_COMPOSITE_DARKEN:
            return SkBlendMode::kDarken;
        case FT_COLR_COMPOSITE_LIGHTEN:
            return SkBlendMode::kLighten;
        case FT_COLR_COMPOSITE_COLOR_DODGE:
            return SkBlendMode::kColorDodge;
        case FT_COLR_COMPOSITE_COLOR_BURN:
            return SkBlendMode::kColorBurn;
        case FT_COLR_COMPOSITE_HARD_LIGHT:
            return SkBlendMode::kHardLight;
        case FT_COLR_COMPOSITE_SOFT_LIGHT:
            return SkBlendMode::kSoftLight;
        case FT_COLR_COMPOSITE_DIFFERENCE:
            return SkBlendMode::kDifference;
        case FT_COLR_COMPOSITE_EXCLUSION:
            return SkBlendMode::kExclusion;
        case FT_COLR_COMPOSITE_MULTIPLY:
            return SkBlendMode::kMultiply;
        case FT_COLR_COMPOSITE_HSL_HUE:
            return SkBlendMode::kHue;
        case FT_COLR_COMPOSITE_HSL_SATURATION:
            return SkBlendMode::kSaturation;
        case FT_COLR_COMPOSITE_HSL_COLOR:
            return SkBlendMode::kColor;
        case FT_COLR_COMPOSITE_HSL_LUMINOSITY:
            return SkBlendMode::kLuminosity;
        default:
            return SkBlendMode::kDst;
    }
}

inline SkMatrix ToSkMatrix(FT_Affine23 affine23) {
    // Convert from FreeType's FT_Affine23 column major order to SkMatrix row-major order.
    return SkMatrix::MakeAll(
         SkFixedToScalar(affine23.xx), -SkFixedToScalar(affine23.xy),  SkFixedToScalar(affine23.dx),
        -SkFixedToScalar(affine23.yx),  SkFixedToScalar(affine23.yy), -SkFixedToScalar(affine23.dy),
         0,                             0,                             1);
}

inline SkPoint SkVectorProjection(SkPoint a, SkPoint b) {
    SkScalar length = b.length();
    if (!length) {
        return SkPoint();
    }
    SkPoint bNormalized = b;
    bNormalized.normalize();
    bNormalized.scale(SkPoint::DotProduct(a, b) / length);
    return bNormalized;
}

bool colrv1_configure_skpaint(FT_Face face,
                              const SkSpan<SkColor>& palette,
                              const SkColor foregroundColor,
                              const FT_COLR_Paint& colrPaint,
                              SkPaint* paint) {
    auto fetchColorStops = [&face, &palette, &foregroundColor](
                                               const FT_ColorStopIterator& colorStopIterator,
                                               std::vector<SkScalar>& stops,
                                               std::vector<SkColor>& colors) -> bool {
        const FT_UInt colorStopCount = colorStopIterator.num_color_stops;
        if (colorStopCount == 0) {
            return false;
        }

        // 5.7.11.2.4 ColorIndex, ColorStop and ColorLine
        // "Applications shall apply the colorStops in increasing stopOffset order."
        struct ColorStop {
            SkScalar pos;
            SkColor color;
        };
        std::vector<ColorStop> colorStopsSorted;
        colorStopsSorted.resize(colorStopCount);

        FT_ColorStop color_stop;
        FT_ColorStopIterator mutable_color_stop_iterator = colorStopIterator;
        while (FT_Get_Colorline_Stops(face, &color_stop, &mutable_color_stop_iterator)) {
            FT_UInt index = mutable_color_stop_iterator.current_color_stop - 1;
            colorStopsSorted[index].pos = color_stop.stop_offset / float(1 << 14);
            FT_UInt16& palette_index = color_stop.color.palette_index;
            if (palette_index == kForegroundColorPaletteIndex) {
                U8CPU newAlpha = SkColorGetA(foregroundColor) *
                                 SkColrV1AlphaToFloat(color_stop.color.alpha);
                colorStopsSorted[index].color = SkColorSetA(foregroundColor, newAlpha);
            } else if (palette_index >= palette.size()) {
                return false;
            } else {
                U8CPU newAlpha = SkColorGetA(palette[palette_index]) *
                                 SkColrV1AlphaToFloat(color_stop.color.alpha);
                colorStopsSorted[index].color = SkColorSetA(palette[palette_index], newAlpha);
            }
        }

        std::stable_sort(colorStopsSorted.begin(), colorStopsSorted.end(),
                         [](const ColorStop& a, const ColorStop& b) { return a.pos < b.pos; });

        stops.resize(colorStopCount);
        colors.resize(colorStopCount);
        for (size_t i = 0; i < colorStopCount; ++i) {
            stops[i] = colorStopsSorted[i].pos;
            colors[i] = colorStopsSorted[i].color;
        }
        return true;
    };

    switch (colrPaint.format) {
        case FT_COLR_PAINTFORMAT_SOLID: {
            FT_PaintSolid solid = colrPaint.u.solid;

            // Dont' draw anything with this color if the palette index is out of bounds.
            SkColor color = SK_ColorTRANSPARENT;
            if (solid.color.palette_index == kForegroundColorPaletteIndex) {
                U8CPU newAlpha = SkColorGetA(foregroundColor) *
                                 SkColrV1AlphaToFloat(solid.color.alpha);
                color = SkColorSetA(foregroundColor, newAlpha);
            } else if (solid.color.palette_index >= palette.size()) {
                return false;
            } else {
                U8CPU newAlpha = SkColorGetA(palette[solid.color.palette_index]) *
                                 SkColrV1AlphaToFloat(solid.color.alpha);
                color = SkColorSetA(palette[solid.color.palette_index], newAlpha);
            }
            paint->setShader(nullptr);
            paint->setColor(color);
            return true;
        }
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT: {
            const FT_PaintLinearGradient& linearGradient = colrPaint.u.linear_gradient;
            std::vector<SkScalar> stops;
            std::vector<SkColor> colors;

            if (!fetchColorStops(linearGradient.colorline.color_stop_iterator, stops, colors)) {
                return false;
            }

            if (stops.size() == 1) {
                paint->setColor(colors[0]);
                return true;
            }

            SkPoint linePositions[2] = {SkPoint::Make( SkFixedToScalar(linearGradient.p0.x),
                                                      -SkFixedToScalar(linearGradient.p0.y)),
                                        SkPoint::Make( SkFixedToScalar(linearGradient.p1.x),
                                                      -SkFixedToScalar(linearGradient.p1.y))};
            SkPoint p0 = linePositions[0];
            SkPoint p1 = linePositions[1];
            SkPoint p2 = SkPoint::Make( SkFixedToScalar(linearGradient.p2.x),
                                       -SkFixedToScalar(linearGradient.p2.y));

            // If p0p1 or p0p2 are degenerate probably nothing should be drawn.
            // If p0p1 and p0p2 are parallel then one side is the first color and the other side is
            // the last color, depending on the direction.
            // For now, just use the first color.
            if (p1 == p0 || p2 == p0 || !SkPoint::CrossProduct(p1 - p0, p2 - p0)) {
                paint->setColor(colors[0]);
                return true;
            }

            // Follow implementation note in nanoemoji:
            // https://github.com/googlefonts/nanoemoji/blob/0ac6e7bb4d8202db692574d8530a9b643f1b3b3c/src/nanoemoji/svg.py#L188
            // to compute a new gradient end point P3 as the orthogonal
            // projection of the vector from p0 to p1 onto a line perpendicular
            // to line p0p2 and passing through p0.
            SkVector perpendicularToP2P0 = (p2 - p0);
            perpendicularToP2P0 = SkPoint::Make( perpendicularToP2P0.y(),
                                                -perpendicularToP2P0.x());
            SkVector p3 = p0 + SkVectorProjection((p1 - p0), perpendicularToP2P0);

            // Project/scale points according to stop extrema along p0p3 line,
            // p3 being the result of the projection above, then scale stops to
            // to [0, 1] range so that repeat modes work.  The Skia linear
            // gradient shader performs the repeat modes over the 0 to 1 range,
            // that's why we need to scale the stops to within that range.
            SkVector p0p3 = p3 - p0;
            SkVector p0Offset = p0p3;
            p0Offset.scale(stops.front());
            SkVector p1Offset = p0p3;
            p1Offset.scale(stops.back());

            linePositions[0] = p0 + p0Offset;
            linePositions[1] = p0 + p1Offset;

            SkScalar scaleFactor = 1 / (stops.back() - stops.front());
            SkScalar startOffset = stops.front();
            for (SkScalar& stop : stops) {
                stop = (stop - startOffset) * scaleFactor;
            }

            sk_sp<SkShader> shader(SkGradientShader::MakeLinear(
                                   linePositions,
                                   colors.data(), stops.data(), stops.size(),
                                   ToSkTileMode(linearGradient.colorline.extend)));
            SkASSERT(shader);
            // An opaque color is needed to ensure the gradient is not modulated by alpha.
            paint->setColor(SK_ColorBLACK);
            paint->setShader(shader);
            return true;
        }
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT: {
            const FT_PaintRadialGradient& radialGradient = colrPaint.u.radial_gradient;
            SkPoint start = SkPoint::Make( SkFixedToScalar(radialGradient.c0.x),
                                          -SkFixedToScalar(radialGradient.c0.y));
            SkScalar startRadius = SkFixedToScalar(radialGradient.r0);
            SkPoint end = SkPoint::Make( SkFixedToScalar(radialGradient.c1.x),
                                        -SkFixedToScalar(radialGradient.c1.y));
            SkScalar endRadius = SkFixedToScalar(radialGradient.r1);


            std::vector<SkScalar> stops;
            std::vector<SkColor> colors;
            if (!fetchColorStops(radialGradient.colorline.color_stop_iterator, stops, colors)) {
                return false;
            }

            if (stops.size() == 1) {
                paint->setColor(colors[0]);
                return true;
            }

            // An opaque color is needed to ensure the gradient is not modulated by alpha.
            paint->setColor(SK_ColorBLACK);

            paint->setShader(SkGradientShader::MakeTwoPointConical(
                    start, startRadius, end, endRadius, colors.data(), stops.data(), stops.size(),
                    ToSkTileMode(radialGradient.colorline.extend)));
            return true;
        }
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
            const FT_PaintSweepGradient& sweepGradient = colrPaint.u.sweep_gradient;
            SkPoint center = SkPoint::Make( SkFixedToScalar(sweepGradient.center.x),
                                           -SkFixedToScalar(sweepGradient.center.y));
            SkScalar startAngle = SkFixedToScalar(sweepGradient.start_angle * 180.0f);
            SkScalar endAngle = SkFixedToScalar(sweepGradient.end_angle * 180.0f);

            std::vector<SkScalar> stops;
            std::vector<SkColor> colors;
            if (!fetchColorStops(sweepGradient.colorline.color_stop_iterator, stops, colors)) {
                return false;
            }

            if (stops.size() == 1) {
                paint->setColor(colors[0]);
                return true;
            }

            // An opaque color is needed to ensure the gradient is not modulated by alpha.
            paint->setColor(SK_ColorBLACK);

            // Prepare angles to be within range for the shader.
            auto clampAngleToRange = [](SkScalar angle) {
                SkScalar clampedAngle = SkScalarMod(angle, 360.f);
                if (clampedAngle < 0) {
                    return clampedAngle + 360.f;
                }
                return clampedAngle;
            };
            startAngle = clampAngleToRange(startAngle);
            endAngle = clampAngleToRange(endAngle);
#ifdef SK_IGNORE_COLRV1_SWEEP_FIX
            /* TODO: Spec clarifications on which side of the gradient is to be
             * painted, repeat modes, how to handle 0 degrees transition, see
             * https://github.com/googlefonts/colr-gradients-spec/issues/250 */
            if (startAngle >= endAngle) {
                endAngle += 360.f;
            }

            // Skia's angles start from the horizontal x-Axis, rotate left 90
            // degrees and then mirror horizontally to correct for Skia angles
            // going clockwise, COLR v1 angles going counterclockwise.
            SkMatrix angleAdjust = SkMatrix::RotateDeg(-90.f, center);
            angleAdjust.postScale(-1, 1, center.x(), center.y());

            paint->setShader(SkGradientShader::MakeSweep(
                    center.x(), center.y(), colors.data(), stops.data(), stops.size(),
                    SkTileMode::kDecal, startAngle, endAngle, 0, &angleAdjust));
#else
            SkScalar sectorAngle =
                    endAngle > startAngle ? endAngle - startAngle : endAngle + 360.0f - startAngle;

            /* https://docs.microsoft.com/en-us/typography/opentype/spec/colr#sweep-gradients
             * "The angles are expressed in counter-clockwise degrees from the
             * direction of the positive x-axis on the design grid. [...]  The
             * color line progresses from the start angle to the end angle in
             * the counter-clockwise direction;"
             */

            SkMatrix localMatrix;
            localMatrix.postRotate(startAngle, center.x(), center.y());
            /* Mirror along x-axis to change angle direction. */
            localMatrix.postScale(1, -1, center.x(), center.y());
            SkTileMode tileMode = ToSkTileMode(sweepGradient.colorline.extend);

            paint->setShader(SkGradientShader::MakeSweep(
                    center.x(), center.y(), colors.data(), stops.data(), stops.size(),
                    tileMode, 0, sectorAngle, 0, &localMatrix));
#endif
            return true;
        }
        default: {
            SkASSERT(false);
            return false;
        }
    }
    SkUNREACHABLE;
}

bool colrv1_draw_paint(SkCanvas* canvas,
                       const SkSpan<SkColor>& palette,
                       const SkColor foregroundColor,
                       FT_Face face,
                       const FT_COLR_Paint& colrPaint) {
    switch (colrPaint.format) {
        case FT_COLR_PAINTFORMAT_GLYPH: {
            FT_UInt glyphID = colrPaint.u.glyph.glyphID;
            SkPath path;
            /* TODO: Currently this call retrieves the path at units_per_em size. If we want to get
             * correct hinting for the scaled size under the transforms at this point in the color
             * glyph graph, we need to extract at least the requested glyph width and height and
             * pass that to the path generation. */
            if (!generateFacePathCOLRv1(face, glyphID, &path)) {
                return false;
            }
            if constexpr (kSkShowTextBlitCoverage) {
                SkPaint highlight_paint;
                highlight_paint.setColor(0x33FF0000);
                canvas->drawRect(path.getBounds(), highlight_paint);
            }
            canvas->clipPath(path, true /* doAntiAlias */);
            return true;
        }
        case FT_COLR_PAINTFORMAT_SOLID:
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
            SkPaint skPaint;
            if (!colrv1_configure_skpaint(face, palette, foregroundColor, colrPaint, &skPaint)) {
                return false;
            }
            canvas->drawPaint(skPaint);
            return true;
        }
        case FT_COLR_PAINTFORMAT_TRANSFORM:
        case FT_COLR_PAINTFORMAT_TRANSLATE:
        case FT_COLR_PAINTFORMAT_SCALE:
        case FT_COLR_PAINTFORMAT_ROTATE:
        case FT_COLR_PAINTFORMAT_SKEW:
            [[fallthrough]];  // Transforms handled in colrv1_transform.
        default:
            SkASSERT(false);
            return false;
    }
    SkUNREACHABLE;
}

bool colrv1_draw_glyph_with_path(SkCanvas* canvas,
                                 const SkSpan<SkColor>& palette, SkColor foregroundColor,
                                 FT_Face face,
                                 const FT_COLR_Paint& glyphPaint, const FT_COLR_Paint& fillPaint) {
    SkASSERT(glyphPaint.format == FT_COLR_PAINTFORMAT_GLYPH);
    SkASSERT(fillPaint.format == FT_COLR_PAINTFORMAT_SOLID ||
             fillPaint.format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT ||
             fillPaint.format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT ||
             fillPaint.format == FT_COLR_PAINTFORMAT_SWEEP_GRADIENT);

    SkPaint skiaFillPaint;
    skiaFillPaint.setAntiAlias(true);
    if (!colrv1_configure_skpaint(face, palette, foregroundColor, fillPaint, &skiaFillPaint)) {
        return false;
    }

    FT_UInt glyphID = glyphPaint.u.glyph.glyphID;
    SkPath path;
    /* TODO: Currently this call retrieves the path at units_per_em size. If we want to get
     * correct hinting for the scaled size under the transforms at this point in the color
     * glyph graph, we need to extract at least the requested glyph width and height and
     * pass that to the path generation. */
    if (!generateFacePathCOLRv1(face, glyphID, &path)) {
        return false;
    }
    if constexpr (kSkShowTextBlitCoverage) {
        SkPaint highlightPaint;
        highlightPaint.setColor(0x33FF0000);
        canvas->drawRect(path.getBounds(), highlightPaint);
    }
    canvas->drawPath(path, skiaFillPaint);
    return true;
}


/* In drawing mode, concatenates the transforms directly on SkCanvas. In
 * bounding box calculation mode, no SkCanvas is specified, but we only want to
 * retrieve the transform from the FreeType paint object. */
void colrv1_transform(FT_Face face,
                      const FT_COLR_Paint& colrPaint,
                      SkCanvas* canvas,
                      SkMatrix* outTransform = nullptr) {
    SkMatrix transform;

    SkASSERT(canvas || outTransform);

    switch (colrPaint.format) {
        case FT_COLR_PAINTFORMAT_TRANSFORM: {
            transform = ToSkMatrix(colrPaint.u.transform.affine);
            break;
        }
        case FT_COLR_PAINTFORMAT_TRANSLATE: {
            transform = SkMatrix::Translate( SkFixedToScalar(colrPaint.u.translate.dx),
                                            -SkFixedToScalar(colrPaint.u.translate.dy));
            break;
        }
        case FT_COLR_PAINTFORMAT_SCALE: {
            transform.setScale( SkFixedToScalar(colrPaint.u.scale.scale_x),
                                SkFixedToScalar(colrPaint.u.scale.scale_y),
                                SkFixedToScalar(colrPaint.u.scale.center_x),
                               -SkFixedToScalar(colrPaint.u.scale.center_y));
            break;
        }
        case FT_COLR_PAINTFORMAT_ROTATE: {
            // COLRv1 angles are counter-clockwise, compare
            // https://docs.microsoft.com/en-us/typography/opentype/spec/colr#formats-24-to-27-paintrotate-paintvarrotate-paintrotatearoundcenter-paintvarrotatearoundcenter
            transform = SkMatrix::RotateDeg(
#ifdef SK_IGNORE_COLRV1_TRANSFORM_FIX
                    SkFixedToScalar(colrPaint.u.rotate.angle) * 180.0f,
#else
                    -SkFixedToScalar(colrPaint.u.rotate.angle) * 180.0f,
#endif
                    SkPoint::Make( SkFixedToScalar(colrPaint.u.rotate.center_x),
                                  -SkFixedToScalar(colrPaint.u.rotate.center_y)));
            break;
        }
        case FT_COLR_PAINTFORMAT_SKEW: {
            // In the PAINTFORMAT_ROTATE implementation, SkMatrix setRotate
            // snaps to 0 for values very close to 0. Do the same here.

            SkScalar xDeg = SkFixedToScalar(colrPaint.u.skew.x_skew_angle) * 180.0f;
#ifdef SK_IGNORE_COLRV1_TRANSFORM_FIX
            SkScalar xRad = SkDegreesToRadians(-xDeg);
#else
            SkScalar xRad = SkDegreesToRadians(xDeg);
#endif
            SkScalar xTan = SkScalarTan(xRad);
            xTan = SkScalarNearlyZero(xTan) ? 0.0f : xTan;

            SkScalar yDeg = SkFixedToScalar(colrPaint.u.skew.y_skew_angle) * 180.0f;
            // Negate y_skew_angle due to Skia's y-down coordinate system to achieve
            // counter-clockwise skew along the y-axis.
            SkScalar yRad = SkDegreesToRadians(-yDeg);
            SkScalar yTan = SkScalarTan(yRad);
            yTan = SkScalarNearlyZero(yTan) ? 0.0f : yTan;

            transform.setSkew(xTan, yTan,
                              SkFixedToScalar(colrPaint.u.skew.center_x),
                             -SkFixedToScalar(colrPaint.u.skew.center_y));
            break;
        }
        default: {
            SkASSERT(false);  // Only transforms are handled in this function.
        }
    }
    if (canvas) {
        canvas->concat(transform);
    }
    if (outTransform) {
        *outTransform = transform;
    }
}

bool colrv1_start_glyph(SkCanvas* canvas,
                        const SkSpan<SkColor>& palette,
                        const SkColor foregroundColor,
                        FT_Face face,
                        uint16_t glyphId,
                        FT_Color_Root_Transform rootTransform,
                        VisitedSet* activePaints);

bool colrv1_traverse_paint(SkCanvas* canvas,
                           const SkSpan<SkColor>& palette,
                           const SkColor foregroundColor,
                           FT_Face face,
                           FT_OpaquePaint opaquePaint,
                           VisitedSet* activePaints) {
    // Cycle detection, see section "5.7.11.1.9 Color glyphs as a directed acyclic graph".
    if (activePaints->contains(opaquePaint)) {
        return false;
    }

    activePaints->add(opaquePaint);
    SK_AT_SCOPE_EXIT(activePaints->remove(opaquePaint));

    FT_COLR_Paint paint;
    if (!FT_Get_Paint(face, opaquePaint, &paint)) {
        return false;
    }

    SkAutoCanvasRestore autoRestore(canvas, true /* doSave */);
    switch (paint.format) {
        case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
            FT_LayerIterator& layerIterator = paint.u.colr_layers.layer_iterator;
            FT_OpaquePaint layerPaint{nullptr, 1};
            while (FT_Get_Paint_Layers(face, &layerIterator, &layerPaint)) {
                if (!colrv1_traverse_paint(canvas, palette, foregroundColor, face,
                                           layerPaint, activePaints)) {
                    return false;
                }
            }
            return true;
        }
        case FT_COLR_PAINTFORMAT_GLYPH:
            // Special case paint graph leaf situations to improve
            // performance. These are situations in the graph where a GlyphPaint
            // is followed by either a solid or a gradient fill. Here we can use
            // drawPath() + SkPaint directly which is faster than setting a
            // clipPath() followed by a drawPaint().
            FT_COLR_Paint fillPaint;
            if (!FT_Get_Paint(face, paint.u.glyph.paint, &fillPaint)) {
                return false;
            }
            if (fillPaint.format == FT_COLR_PAINTFORMAT_SOLID ||
                fillPaint.format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT ||
                fillPaint.format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT ||
                fillPaint.format == FT_COLR_PAINTFORMAT_SWEEP_GRADIENT)
            {
                return colrv1_draw_glyph_with_path(canvas, palette, foregroundColor,
                                                   face, paint, fillPaint);
            }
            if (!colrv1_draw_paint(canvas, palette, foregroundColor, face, paint)) {
                return false;
            }
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.glyph.paint, activePaints);
        case FT_COLR_PAINTFORMAT_COLR_GLYPH:
            return colrv1_start_glyph(canvas, palette, foregroundColor,
                                      face, paint.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM,
                                      activePaints);
        case FT_COLR_PAINTFORMAT_TRANSFORM:
            colrv1_transform(face, paint, canvas);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.transform.paint, activePaints);
        case FT_COLR_PAINTFORMAT_TRANSLATE:
            colrv1_transform(face, paint, canvas);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.translate.paint, activePaints);
        case FT_COLR_PAINTFORMAT_SCALE:
            colrv1_transform(face, paint, canvas);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.scale.paint, activePaints);
        case FT_COLR_PAINTFORMAT_ROTATE:
            colrv1_transform(face, paint, canvas);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.rotate.paint, activePaints);
        case FT_COLR_PAINTFORMAT_SKEW:
            colrv1_transform(face, paint, canvas);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.skew.paint, activePaints);
        case FT_COLR_PAINTFORMAT_COMPOSITE: {
            SkAutoCanvasRestore acr(canvas, false);
            canvas->saveLayer(nullptr, nullptr);
            if (!colrv1_traverse_paint(canvas, palette, foregroundColor,
                                       face, paint.u.composite.backdrop_paint, activePaints)) {
                return false;
            }
            SkPaint blendModePaint;
            blendModePaint.setBlendMode(ToSkBlendMode(paint.u.composite.composite_mode));
            canvas->saveLayer(nullptr, &blendModePaint);
            return colrv1_traverse_paint(canvas, palette, foregroundColor,
                                         face, paint.u.composite.source_paint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_SOLID:
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
            return colrv1_draw_paint(canvas, palette, foregroundColor, face, paint);
        }
        default:
            SkASSERT(false);
            return false;
    }
    SkUNREACHABLE;
}

SkPath GetClipBoxPath(FT_Face face, uint16_t glyphId, bool untransformed) {
    SkPath resultPath;

    using DoneFTSize = SkFunctionWrapper<decltype(FT_Done_Size), FT_Done_Size>;
    std::unique_ptr<std::remove_pointer_t<FT_Size>, DoneFTSize> unscaledFtSize = nullptr;

    FT_Size oldSize = face->size;
    FT_Matrix oldTransform;
    FT_Vector oldDelta;
    FT_Error err = 0;

    if (untransformed) {
        unscaledFtSize.reset(
                [face]() -> FT_Size {
                    FT_Size size;
                    FT_Error err = FT_New_Size(face, &size);
                    if (err != 0) {
                        SK_TRACEFTR(err,
                                    "FT_New_Size(%s) failed in generateFacePathStaticCOLRv1.",
                                    face->family_name);
                        return nullptr;
                    }
                    return size;
                }());
        if (!unscaledFtSize) {
            return resultPath;
        }

        err = FT_Activate_Size(unscaledFtSize.get());
        if (err != 0) {
            return resultPath;
        }

        err = FT_Set_Char_Size(face, SkIntToFDot6(face->units_per_EM), 0, 0, 0);
        if (err != 0) {
            return resultPath;
        }

        FT_Get_Transform(face, &oldTransform, &oldDelta);
        FT_Set_Transform(face, nullptr, nullptr);
    }

    FT_ClipBox colrGlyphClipBox;
    if (FT_Get_Color_Glyph_ClipBox(face, glyphId, &colrGlyphClipBox)) {
        resultPath = SkPath::Polygon({{ SkFDot6ToScalar(colrGlyphClipBox.bottom_left.x),
                                       -SkFDot6ToScalar(colrGlyphClipBox.bottom_left.y)},
                                      { SkFDot6ToScalar(colrGlyphClipBox.top_left.x),
                                       -SkFDot6ToScalar(colrGlyphClipBox.top_left.y)},
                                      { SkFDot6ToScalar(colrGlyphClipBox.top_right.x),
                                       -SkFDot6ToScalar(colrGlyphClipBox.top_right.y)},
                                      { SkFDot6ToScalar(colrGlyphClipBox.bottom_right.x),
                                       -SkFDot6ToScalar(colrGlyphClipBox.bottom_right.y)}},
                                     true);
    }

    if (untransformed) {
        err = FT_Activate_Size(oldSize);
        if (err != 0) {
          return resultPath;
        }
        FT_Set_Transform(face, &oldTransform, &oldDelta);
    }

    return resultPath;
}

bool colrv1_start_glyph(SkCanvas* canvas,
                        const SkSpan<SkColor>& palette,
                        const SkColor foregroundColor,
                        FT_Face face,
                        uint16_t glyphId,
                        FT_Color_Root_Transform rootTransform,
                        VisitedSet* activePaints) {
    FT_OpaquePaint opaquePaint{nullptr, 1};
    if (!FT_Get_Color_Glyph_Paint(face, glyphId, rootTransform, &opaquePaint)) {
        return false;
    }

    bool untransformed = rootTransform == FT_COLOR_NO_ROOT_TRANSFORM;
    SkPath clipBoxPath = GetClipBoxPath(face, glyphId, untransformed);
    if (!clipBoxPath.isEmpty()) {
        canvas->clipPath(clipBoxPath, true);
    }

    if (!colrv1_traverse_paint(canvas, palette, foregroundColor,
                               face, opaquePaint, activePaints)) {
        return false;
    }

    return true;
}

bool colrv1_start_glyph_bounds(SkMatrix *ctm,
                               SkRect* bounds,
                               FT_Face face,
                               uint16_t glyphId,
                               FT_Color_Root_Transform rootTransform,
                               VisitedSet* activePaints);

bool colrv1_traverse_paint_bounds(SkMatrix* ctm,
                                  SkRect* bounds,
                                  FT_Face face,
                                  FT_OpaquePaint opaquePaint,
                                  VisitedSet* activePaints) {
    // Cycle detection, see section "5.7.11.1.9 Color glyphs as a directed acyclic graph".
    if (activePaints->contains(opaquePaint)) {
        return false;
    }

    activePaints->add(opaquePaint);
    SK_AT_SCOPE_EXIT(activePaints->remove(opaquePaint));

    FT_COLR_Paint paint;
    if (!FT_Get_Paint(face, opaquePaint, &paint)) {
        return false;
    }

    SkMatrix restoreMatrix = *ctm;
    SK_AT_SCOPE_EXIT(*ctm = restoreMatrix);

    switch (paint.format) {
        case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
            FT_LayerIterator& layerIterator = paint.u.colr_layers.layer_iterator;
            FT_OpaquePaint layerPaint{nullptr, 1};
            while (FT_Get_Paint_Layers(face, &layerIterator, &layerPaint)) {
                if (!colrv1_traverse_paint_bounds(ctm, bounds, face, layerPaint, activePaints)) {
                    return false;
                }
            }
            return true;
        }
        case FT_COLR_PAINTFORMAT_GLYPH: {
            FT_UInt glyphID = paint.u.glyph.glyphID;
            SkPath path;
            if (!generateFacePathCOLRv1(face, glyphID, &path)) {
                return false;
            }
            path.transform(*ctm);
            bounds->join(path.getBounds());
            return true;
        }
        case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
            FT_UInt glyphID = paint.u.colr_glyph.glyphID;
            return colrv1_start_glyph_bounds(ctm, bounds, face, glyphID, FT_COLOR_NO_ROOT_TRANSFORM,
                                             activePaints);
        }
        case FT_COLR_PAINTFORMAT_TRANSFORM: {
            SkMatrix transformMatrix;
            colrv1_transform(face, paint, nullptr, &transformMatrix);
            ctm->preConcat(transformMatrix);
            FT_OpaquePaint& transformPaint = paint.u.transform.paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, transformPaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_TRANSLATE: {
            SkMatrix transformMatrix;
            colrv1_transform(face, paint, nullptr, &transformMatrix);
            ctm->preConcat(transformMatrix);
            FT_OpaquePaint& translatePaint = paint.u.translate.paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, translatePaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_SCALE: {
            SkMatrix transformMatrix;
            colrv1_transform(face, paint, nullptr, &transformMatrix);
            ctm->preConcat(transformMatrix);
            FT_OpaquePaint& scalePaint = paint.u.scale.paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, scalePaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_ROTATE: {
            SkMatrix transformMatrix;
            colrv1_transform(face, paint, nullptr, &transformMatrix);
            ctm->preConcat(transformMatrix);
            FT_OpaquePaint& rotatePaint = paint.u.rotate.paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, rotatePaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_SKEW: {
            SkMatrix transformMatrix;
            colrv1_transform(face, paint, nullptr, &transformMatrix);
            ctm->preConcat(transformMatrix);
            FT_OpaquePaint& skewPaint = paint.u.skew.paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, skewPaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_COMPOSITE: {
            FT_OpaquePaint& backdropPaint = paint.u.composite.backdrop_paint;
            FT_OpaquePaint&   sourcePaint = paint.u.composite.  source_paint;
            return colrv1_traverse_paint_bounds(ctm, bounds, face, backdropPaint, activePaints) &&
                   colrv1_traverse_paint_bounds(ctm, bounds, face,   sourcePaint, activePaints);
        }
        case FT_COLR_PAINTFORMAT_SOLID:
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
            return true;
        }
        default:
            SkASSERT(false);
            return false;
    }
    SkUNREACHABLE;
}


bool colrv1_start_glyph_bounds(SkMatrix *ctm,
                               SkRect* bounds,
                               FT_Face face,
                               uint16_t glyphId,
                               FT_Color_Root_Transform rootTransform,
                               VisitedSet* activePaints) {
    FT_OpaquePaint opaquePaint{nullptr, 1};
    return FT_Get_Color_Glyph_Paint(face, glyphId, rootTransform, &opaquePaint) &&
           colrv1_traverse_paint_bounds(ctm, bounds, face, opaquePaint, activePaints);
}
#endif // TT_SUPPORT_COLRV1

}  // namespace


#ifdef TT_SUPPORT_COLRV1
bool SkScalerContext_FreeType_Base::drawCOLRv1Glyph(FT_Face face,
                                                    const SkGlyph& glyph,
                                                    uint32_t loadGlyphFlags,
                                                    SkSpan<SkColor> palette,
                                                    SkCanvas* canvas) {
    if (this->isSubpixel()) {
        canvas->translate(SkFixedToScalar(glyph.getSubXFixed()),
                          SkFixedToScalar(glyph.getSubYFixed()));
    }

    VisitedSet activePaints;
    bool haveLayers =  colrv1_start_glyph(canvas, palette,
                                          fRec.fForegroundColor,
                                          face, glyph.getGlyphID(),
                                          FT_COLOR_INCLUDE_ROOT_TRANSFORM,
                                          &activePaints);
    SkASSERTF(haveLayers, "Could not get COLRv1 layers from '%s'.", face->family_name);
    return haveLayers;
}
#endif  // TT_SUPPORT_COLRV1

#ifdef FT_COLOR_H
bool SkScalerContext_FreeType_Base::drawCOLRv0Glyph(FT_Face face,
                                                    const SkGlyph& glyph,
                                                    uint32_t loadGlyphFlags,
                                                    SkSpan<SkColor> palette,
                                                    SkCanvas* canvas) {
    if (this->isSubpixel()) {
        canvas->translate(SkFixedToScalar(glyph.getSubXFixed()),
                          SkFixedToScalar(glyph.getSubYFixed()));
    }

    bool haveLayers = false;
    FT_LayerIterator layerIterator;
    layerIterator.p = nullptr;
    FT_UInt layerGlyphIndex = 0;
    FT_UInt layerColorIndex = 0;
    SkPaint paint;
    paint.setAntiAlias(!(loadGlyphFlags & FT_LOAD_TARGET_MONO));
    while (FT_Get_Color_Glyph_Layer(face, glyph.getGlyphID(), &layerGlyphIndex,
                                    &layerColorIndex, &layerIterator)) {
        haveLayers = true;
        if (layerColorIndex == 0xFFFF) {
            paint.setColor(fRec.fForegroundColor);
        } else {
            paint.setColor(palette[layerColorIndex]);
        }
        SkPath path;
        if (this->generateFacePath(face, layerGlyphIndex, loadGlyphFlags, &path)) {
            canvas->drawPath(path, paint);
        }
    }
    SkASSERTF(haveLayers, "Could not get COLRv0 layers from '%s'.", face->family_name);
    return haveLayers;
}
#endif  // FT_COLOR_H

void SkScalerContext_FreeType_Base::generateGlyphImage(FT_Face face,
                                                       const SkGlyph& glyph,
                                                       const SkMatrix& bitmapTransform)
{
    switch ( face->glyph->format ) {
        case FT_GLYPH_FORMAT_OUTLINE: {
            FT_Outline* outline = &face->glyph->outline;

            int dx = 0, dy = 0;
            if (this->isSubpixel()) {
                dx = SkFixedToFDot6(glyph.getSubXFixed());
                dy = SkFixedToFDot6(glyph.getSubYFixed());
                // negate dy since freetype-y-goes-up and skia-y-goes-down
                dy = -dy;
            }

            memset(glyph.fImage, 0, glyph.rowBytes() * glyph.fHeight);

            if (SkMask::kLCD16_Format == glyph.fMaskFormat) {
                const bool doBGR = SkToBool(fRec.fFlags & SkScalerContext::kLCD_BGROrder_Flag);
                const bool doVert = SkToBool(fRec.fFlags & SkScalerContext::kLCD_Vertical_Flag);

                FT_Outline_Translate(outline, dx, dy);
                FT_Error err = FT_Render_Glyph(face->glyph, doVert ? FT_RENDER_MODE_LCD_V :
                                                                     FT_RENDER_MODE_LCD);
                if (err) {
                    SK_TRACEFTR(err, "Could not render glyph %p.", face->glyph);
                    return;
                }

                SkMask mask = glyph.mask();
                if constexpr (kSkShowTextBlitCoverage) {
                    memset(mask.fImage, 0x80, mask.fBounds.height() * mask.fRowBytes);
                }
                FT_GlyphSlotRec& ftGlyph = *face->glyph;

                if (!SkIRect::Intersects(mask.fBounds,
                                         SkIRect::MakeXYWH( ftGlyph.bitmap_left,
                                                           -ftGlyph.bitmap_top,
                                                            ftGlyph.bitmap.width,
                                                            ftGlyph.bitmap.rows)))
                {
                    return;
                }

                // If the FT_Bitmap extent is larger, discard bits of the bitmap outside the mask.
                // If the SkMask extent is larger, shrink mask to fit bitmap (clearing discarded).
                unsigned char* origBuffer = ftGlyph.bitmap.buffer;
                // First align the top left (origin).
                if (-ftGlyph.bitmap_top < mask.fBounds.fTop) {
                    int32_t topDiff = mask.fBounds.fTop - (-ftGlyph.bitmap_top);
                    ftGlyph.bitmap.buffer += ftGlyph.bitmap.pitch * topDiff;
                    ftGlyph.bitmap.rows -= topDiff;
                    ftGlyph.bitmap_top = -mask.fBounds.fTop;
                }
                if (ftGlyph.bitmap_left < mask.fBounds.fLeft) {
                    int32_t leftDiff = mask.fBounds.fLeft - ftGlyph.bitmap_left;
                    ftGlyph.bitmap.buffer += leftDiff;
                    ftGlyph.bitmap.width -= leftDiff;
                    ftGlyph.bitmap_left = mask.fBounds.fLeft;
                }
                if (mask.fBounds.fTop < -ftGlyph.bitmap_top) {
                    mask.fImage += mask.fRowBytes * (-ftGlyph.bitmap_top - mask.fBounds.fTop);
                    mask.fBounds.fTop = -ftGlyph.bitmap_top;
                }
                if (mask.fBounds.fLeft < ftGlyph.bitmap_left) {
                    mask.fImage += sizeof(uint16_t) * (ftGlyph.bitmap_left - mask.fBounds.fLeft);
                    mask.fBounds.fLeft = ftGlyph.bitmap_left;
                }
                // Origins aligned, clean up the width and height.
                int ftVertScale = (doVert ? 3 : 1);
                int ftHoriScale = (doVert ? 1 : 3);
                if (mask.fBounds.height() * ftVertScale < SkToInt(ftGlyph.bitmap.rows)) {
                    ftGlyph.bitmap.rows = mask.fBounds.height() * ftVertScale;
                }
                if (mask.fBounds.width() * ftHoriScale < SkToInt(ftGlyph.bitmap.width)) {
                    ftGlyph.bitmap.width = mask.fBounds.width() * ftHoriScale;
                }
                if (SkToInt(ftGlyph.bitmap.rows) < mask.fBounds.height() * ftVertScale) {
                    mask.fBounds.fBottom = mask.fBounds.fTop + ftGlyph.bitmap.rows / ftVertScale;
                }
                if (SkToInt(ftGlyph.bitmap.width) < mask.fBounds.width() * ftHoriScale) {
                    mask.fBounds.fRight = mask.fBounds.fLeft + ftGlyph.bitmap.width / ftHoriScale;
                }
                if (fPreBlend.isApplicable()) {
                    copyFT2LCD16<true>(ftGlyph.bitmap, mask, doBGR,
                                       fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                } else {
                    copyFT2LCD16<false>(ftGlyph.bitmap, mask, doBGR,
                                        fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                }
                // Restore the buffer pointer so FreeType can properly free it.
                ftGlyph.bitmap.buffer = origBuffer;
            } else {
                FT_BBox     bbox;
                FT_Bitmap   target;
                FT_Outline_Get_CBox(outline, &bbox);
                /*
                    what we really want to do for subpixel is
                        offset(dx, dy)
                        compute_bounds
                        offset(bbox & !63)
                    but that is two calls to offset, so we do the following, which
                    achieves the same thing with only one offset call.
                */
                FT_Outline_Translate(outline, dx - ((bbox.xMin + dx) & ~63),
                                              dy - ((bbox.yMin + dy) & ~63));

                target.width = glyph.fWidth;
                target.rows = glyph.fHeight;
                target.pitch = glyph.rowBytes();
                target.buffer = reinterpret_cast<uint8_t*>(glyph.fImage);
                target.pixel_mode = compute_pixel_mode(glyph.fMaskFormat);
                target.num_grays = 256;

                FT_Outline_Get_Bitmap(face->glyph->library, outline, &target);
                if constexpr (kSkShowTextBlitCoverage) {
                    if (glyph.fMaskFormat == SkMask::kBW_Format) {
                        for (unsigned y = 0; y < target.rows; y += 2) {
                            for (unsigned x = (y & 0x2); x < target.width; x+=4) {
                                uint8_t& b = target.buffer[(target.pitch * y) + (x >> 3)];
                                b = b ^ (1 << (0x7 - (x & 0x7)));
                            }
                        }
                    } else {
                        for (unsigned y = 0; y < target.rows; ++y) {
                            for (unsigned x = 0; x < target.width; ++x) {
                                uint8_t& a = target.buffer[(target.pitch * y) + x];
                                a = std::max<uint8_t>(a, 0x20);
                            }
                        }
                    }
                }
            }
        } break;

        case FT_GLYPH_FORMAT_BITMAP: {
            FT_Pixel_Mode pixel_mode = static_cast<FT_Pixel_Mode>(face->glyph->bitmap.pixel_mode);
            SkMask::Format maskFormat = static_cast<SkMask::Format>(glyph.fMaskFormat);

            // Assume that the other formats do not exist.
            SkASSERT(FT_PIXEL_MODE_MONO == pixel_mode ||
                     FT_PIXEL_MODE_GRAY == pixel_mode ||
                     FT_PIXEL_MODE_BGRA == pixel_mode);

            // These are the only formats this ScalerContext should request.
            SkASSERT(SkMask::kBW_Format == maskFormat ||
                     SkMask::kA8_Format == maskFormat ||
                     SkMask::kARGB32_Format == maskFormat ||
                     SkMask::kLCD16_Format == maskFormat);

            // If no scaling needed, directly copy glyph bitmap.
            if (bitmapTransform.isIdentity()) {
                SkMask dstMask = glyph.mask();
                copyFTBitmap(face->glyph->bitmap, dstMask);
                break;
            }

            // Otherwise, scale the bitmap.

            // Copy the FT_Bitmap into an SkBitmap (either A8 or ARGB)
            SkBitmap unscaledBitmap;
            // TODO: mark this as sRGB when the blits will be sRGB.
            unscaledBitmap.allocPixels(SkImageInfo::Make(face->glyph->bitmap.width,
                                                         face->glyph->bitmap.rows,
                                                         SkColorType_for_FTPixelMode(pixel_mode),
                                                         kPremul_SkAlphaType));

            SkMask unscaledBitmapAlias;
            unscaledBitmapAlias.fImage = reinterpret_cast<uint8_t*>(unscaledBitmap.getPixels());
            unscaledBitmapAlias.fBounds.setWH(unscaledBitmap.width(), unscaledBitmap.height());
            unscaledBitmapAlias.fRowBytes = unscaledBitmap.rowBytes();
            unscaledBitmapAlias.fFormat = SkMaskFormat_for_SkColorType(unscaledBitmap.colorType());
            copyFTBitmap(face->glyph->bitmap, unscaledBitmapAlias);

            // Wrap the glyph's mask in a bitmap, unless the glyph's mask is BW or LCD.
            // BW requires an A8 target for resizing, which can then be down sampled.
            // LCD should use a 4x A8 target, which will then be down sampled.
            // For simplicity, LCD uses A8 and is replicated.
            int bitmapRowBytes = 0;
            if (SkMask::kBW_Format != maskFormat && SkMask::kLCD16_Format != maskFormat) {
                bitmapRowBytes = glyph.rowBytes();
            }
            SkBitmap dstBitmap;
            // TODO: mark this as sRGB when the blits will be sRGB.
            dstBitmap.setInfo(SkImageInfo::Make(glyph.fWidth, glyph.fHeight,
                                                SkColorType_for_SkMaskFormat(maskFormat),
                                                kPremul_SkAlphaType),
                              bitmapRowBytes);
            if (SkMask::kBW_Format == maskFormat || SkMask::kLCD16_Format == maskFormat) {
                dstBitmap.allocPixels();
            } else {
                dstBitmap.setPixels(glyph.fImage);
            }

            // Scale unscaledBitmap into dstBitmap.
            SkCanvas canvas(dstBitmap);
            if constexpr (kSkShowTextBlitCoverage) {
                canvas.clear(0x33FF0000);
            } else {
                canvas.clear(SK_ColorTRANSPARENT);
            }
            canvas.translate(-glyph.fLeft, -glyph.fTop);
            canvas.concat(bitmapTransform);
            canvas.translate(face->glyph->bitmap_left, -face->glyph->bitmap_top);

            SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kNearest);
            canvas.drawImage(unscaledBitmap.asImage().get(), 0, 0, sampling, nullptr);

            // If the destination is BW or LCD, convert from A8.
            if (SkMask::kBW_Format == maskFormat) {
                // Copy the A8 dstBitmap into the A1 glyph.fImage.
                SkMask dstMask = glyph.mask();
                packA8ToA1(dstMask, dstBitmap.getAddr8(0, 0), dstBitmap.rowBytes());
            } else if (SkMask::kLCD16_Format == maskFormat) {
                // Copy the A8 dstBitmap into the LCD16 glyph.fImage.
                uint8_t* src = dstBitmap.getAddr8(0, 0);
                uint16_t* dst = reinterpret_cast<uint16_t*>(glyph.fImage);
                for (int y = dstBitmap.height(); y --> 0;) {
                    for (int x = 0; x < dstBitmap.width(); ++x) {
                        dst[x] = grayToRGB16(src[x]);
                    }
                    dst = (uint16_t*)((char*)dst + glyph.rowBytes());
                    src += dstBitmap.rowBytes();
                }
            }

        } break;

        default:
            SkDEBUGFAIL("unknown glyph format");
            memset(glyph.fImage, 0, glyph.rowBytes() * glyph.fHeight);
            return;
    }

// We used to always do this pre-USE_COLOR_LUMINANCE, but with colorlum,
// it is optional
#if defined(SK_GAMMA_APPLY_TO_A8)
    if (SkMask::kA8_Format == glyph.fMaskFormat && fPreBlend.isApplicable()) {
        uint8_t* SK_RESTRICT dst = (uint8_t*)glyph.fImage;
        unsigned rowBytes = glyph.rowBytes();

        for (int y = glyph.fHeight - 1; y >= 0; --y) {
            for (int x = glyph.fWidth - 1; x >= 0; --x) {
                dst[x] = fPreBlend.fG[dst[x]];
            }
            dst += rowBytes;
        }
    }
#endif
}

///////////////////////////////////////////////////////////////////////////////

namespace {

class SkFTGeometrySink {
    SkPath* fPath;
    bool fStarted;
    FT_Vector fCurrent;

    void goingTo(const FT_Vector* pt) {
        if (!fStarted) {
            fStarted = true;
            fPath->moveTo(SkFDot6ToScalar(fCurrent.x), -SkFDot6ToScalar(fCurrent.y));
        }
        fCurrent = *pt;
    }

    bool currentIsNot(const FT_Vector* pt) {
        return fCurrent.x != pt->x || fCurrent.y != pt->y;
    }

    static int Move(const FT_Vector* pt, void* ctx) {
        SkFTGeometrySink& self = *(SkFTGeometrySink*)ctx;
        if (self.fStarted) {
            self.fPath->close();
            self.fStarted = false;
        }
        self.fCurrent = *pt;
        return 0;
    }

    static int Line(const FT_Vector* pt, void* ctx) {
        SkFTGeometrySink& self = *(SkFTGeometrySink*)ctx;
        if (self.currentIsNot(pt)) {
            self.goingTo(pt);
            self.fPath->lineTo(SkFDot6ToScalar(pt->x), -SkFDot6ToScalar(pt->y));
        }
        return 0;
    }

    static int Quad(const FT_Vector* pt0, const FT_Vector* pt1, void* ctx) {
        SkFTGeometrySink& self = *(SkFTGeometrySink*)ctx;
        if (self.currentIsNot(pt0) || self.currentIsNot(pt1)) {
            self.goingTo(pt1);
            self.fPath->quadTo(SkFDot6ToScalar(pt0->x), -SkFDot6ToScalar(pt0->y),
                               SkFDot6ToScalar(pt1->x), -SkFDot6ToScalar(pt1->y));
        }
        return 0;
    }

    static int Cubic(const FT_Vector* pt0, const FT_Vector* pt1, const FT_Vector* pt2, void* ctx) {
        SkFTGeometrySink& self = *(SkFTGeometrySink*)ctx;
        if (self.currentIsNot(pt0) || self.currentIsNot(pt1) || self.currentIsNot(pt2)) {
            self.goingTo(pt2);
            self.fPath->cubicTo(SkFDot6ToScalar(pt0->x), -SkFDot6ToScalar(pt0->y),
                                SkFDot6ToScalar(pt1->x), -SkFDot6ToScalar(pt1->y),
                                SkFDot6ToScalar(pt2->x), -SkFDot6ToScalar(pt2->y));
        }
        return 0;
    }

public:
    SkFTGeometrySink(SkPath* path) : fPath{path}, fStarted{false}, fCurrent{0,0} {}

    inline static constexpr const FT_Outline_Funcs Funcs{
        /*move_to =*/ SkFTGeometrySink::Move,
        /*line_to =*/ SkFTGeometrySink::Line,
        /*conic_to =*/ SkFTGeometrySink::Quad,
        /*cubic_to =*/ SkFTGeometrySink::Cubic,
        /*shift = */ 0,
        /*delta =*/ 0,
    };
};

bool generateGlyphPathStatic(FT_Face face, SkPath* path) {
    SkFTGeometrySink sink{path};
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE ||
        FT_Outline_Decompose(&face->glyph->outline, &SkFTGeometrySink::Funcs, &sink))
    {
        path->reset();
        return false;
    }
    path->close();
    return true;
}

bool generateFacePathStatic(FT_Face face, SkGlyphID glyphID, uint32_t loadGlyphFlags, SkPath* path){
    loadGlyphFlags |= FT_LOAD_NO_BITMAP; // ignore embedded bitmaps so we're sure to get the outline
    loadGlyphFlags &= ~FT_LOAD_RENDER;   // don't scan convert (we just want the outline)
    if (FT_Load_Glyph(face, glyphID, loadGlyphFlags)) {
        path->reset();
        return false;
    }
    return generateGlyphPathStatic(face, path);
}

#ifdef TT_SUPPORT_COLRV1
bool generateFacePathCOLRv1(FT_Face face, SkGlyphID glyphID, SkPath* path) {
    uint32_t flags = 0;
    flags |= FT_LOAD_NO_BITMAP; // ignore embedded bitmaps so we're sure to get the outline
    flags &= ~FT_LOAD_RENDER;   // don't scan convert (we just want the outline)
    flags |= FT_LOAD_NO_HINTING;
    flags |= FT_LOAD_NO_AUTOHINT;
    flags |= FT_LOAD_IGNORE_TRANSFORM;

    using DoneFTSize = SkFunctionWrapper<decltype(FT_Done_Size), FT_Done_Size>;
    std::unique_ptr<std::remove_pointer_t<FT_Size>, DoneFTSize> unscaledFtSize([face]() -> FT_Size {
        FT_Size size;
        FT_Error err = FT_New_Size(face, &size);
        if (err != 0) {
            SK_TRACEFTR(err, "FT_New_Size(%s) failed in generateFacePathStaticCOLRv1.",
                        face->family_name);
            return nullptr;
        }
        return size;
    }());

    if (!unscaledFtSize) {
      return false;
    }

    FT_Size oldSize = face->size;

    auto tryGeneratePath = [face, &unscaledFtSize, glyphID, flags, path]() {
        FT_Error err = 0;

        err = FT_Activate_Size(unscaledFtSize.get());
        if (err != 0) {
          return false;
        }

        err = FT_Set_Char_Size(face, SkIntToFDot6(face->units_per_EM),
                                     SkIntToFDot6(face->units_per_EM), 72, 72);
        if (err != 0) {
            return false;
        }

        err = FT_Load_Glyph(face, glyphID, flags);
        if (err != 0) {
            path->reset();
            return false;
        }

        if (!generateGlyphPathStatic(face, path)) {
            path->reset();
            return false;
        }

        return true;
    };

    bool pathGenerationResult = tryGeneratePath();

    FT_Activate_Size(oldSize);

    return pathGenerationResult;
}
#endif

}  // namespace

bool SkScalerContext_FreeType_Base::generateGlyphPath(FT_Face face, SkPath* path) {
    if (!generateGlyphPathStatic(face, path)) {
        return false;
    }
    if (face->glyph->outline.flags & FT_OUTLINE_OVERLAP) {
        Simplify(*path, path);
    }
    return true;
}

bool SkScalerContext_FreeType_Base::generateFacePath(FT_Face face,
                                                     SkGlyphID glyphID,
                                                     uint32_t loadGlyphFlags,
                                                     SkPath* path) {
    return generateFacePathStatic(face, glyphID, loadGlyphFlags, path);
}

bool SkScalerContext_FreeType_Base::computeColrV1GlyphBoundingBox(FT_Face face,
                                                                  SkGlyphID glyphID,
                                                                  FT_BBox* boundingBox) {
#ifdef TT_SUPPORT_COLRV1
    SkMatrix ctm;
    SkRect bounds = SkRect::MakeEmpty();
    VisitedSet activePaints;
    if (!colrv1_start_glyph_bounds(&ctm, &bounds, face, glyphID,
                                   FT_COLOR_INCLUDE_ROOT_TRANSFORM, &activePaints)) {
        return false;
    }

    /* Convert back to FT_BBox as caller needs it in this format. */
    bounds.sort();
    boundingBox->xMin = SkScalarToFDot6(bounds.left());
    boundingBox->xMax = SkScalarToFDot6(bounds.right());
    boundingBox->yMin = SkScalarToFDot6(-bounds.bottom());
    boundingBox->yMax = SkScalarToFDot6(-bounds.top());

    return true;
#else
    SkASSERT(false);
    return false;
#endif
}
