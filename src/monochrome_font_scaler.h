#ifndef MONOCHROME_FONT_SCALER_H
#define MONOCHROME_FONT_SCALER_H

namespace fallout {

// A borrowed view of a downscaled monochrome glyph. Coverage values use the
// same 0..7 opacity levels as Fallout's interface font blend tables.
//
// The view remains valid until the next call to this function or until
// monochromeFontScalerClearCache is called. The font system is single-threaded,
// so callers should consume the view immediately and never retain it.
typedef struct MonochromeGlyphCoverageView {
    int width;
    int height;
    const unsigned char* coverage;
} MonochromeGlyphCoverageView;

bool monochromeFontGetDownscaledGlyphCoverage(const unsigned char* source,
    int sourceWidth,
    int sourceHeight,
    int sourcePitch,
    int targetWidth,
    int targetHeight,
    MonochromeGlyphCoverageView* view);

void monochromeFontScalerClearCache();

} // namespace fallout

#endif /* MONOCHROME_FONT_SCALER_H */
