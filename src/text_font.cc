#include "text_font.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "color.h"
#include "db.h"
#include "memory.h"
#include "monochrome_font_scaler.h"
#include "platform_compat.h"
#include "settings.h"
#include "window_manager.h"

#include <assert.h>

namespace fallout {

// The maximum number of text fonts.
#define TEXT_FONT_MAX (10)

// The maximum number of font managers.
#define FONT_MANAGER_MAX (10)

typedef struct TextFontGlyph {
    // The width of the glyph in pixels.
    int width;

    // Data offset into [TextFont.data].
    int dataOffset;
} TextFontGlyph;

typedef struct TextFontDescriptor {
    // The number of glyphs in the font.
    int glyphCount;

    // The height of the font.
    int lineHeight;

    // Horizontal spacing between characters in pixels.
    int letterSpacing;

    TextFontGlyph* glyphs;
    unsigned char* data;
    int dataSize;
    int maxGlyphWidth;
} TextFontDescriptor;

static void textFontSetCurrentImpl(int font);
static bool fontManagerFind(int font, FontManager** fontManagerPtr);
static void textFontDrawImpl(unsigned char* buf, const char* string, int length, int pitch, int color);
static int textFontGetLineHeightImpl();
static int textFontGeStringWidthImpl(const char* string);
static int textFontGetCharacterWidthImpl(int ch);
static int textFontGetMonospacedStringWidthImpl(const char* string);
static int textFontGetLetterSpacingImpl();
static int textFontGetBufferSizeImpl(const char* string);
static int textFontGetMonospacedCharacterWidthImpl();
static int textFontDecodeCharacterImpl(const char* string, int* length);
static bool textFontUsesDbcs();
static bool textFontGetGlyphView(const TextFontDescriptor* fontDescriptor, int ch, TextFontGlyphView* glyphView);
static bool textFontGetCurrentGlyphView(int ch, TextFontGlyphView* glyphView);
static int textFontGetScaledGlyphWidth(const TextFontGlyphView& glyphView);
static void textFontDrawGlyph(unsigned char* buf, int pitch, int color, unsigned char* palette, bool isDbcs, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight);

// 0x4D5530 GNW_text_functions
FontManager gTextFontManager = {
    0,
    9,
    textFontSetCurrentImpl,
    textFontDrawImpl,
    textFontGetLineHeightImpl,
    textFontGeStringWidthImpl,
    textFontGetCharacterWidthImpl,
    textFontGetMonospacedStringWidthImpl,
    textFontGetLetterSpacingImpl,
    textFontGetBufferSizeImpl,
    textFontGetMonospacedCharacterWidthImpl,
    textFontDecodeCharacterImpl,
};

// 0x51E3B0 curr_font_num
int gCurrentFont = -1;

// 0x51E3B4 total_managers
int gFontManagersCount = 0;

// 0x51E3B8 text_to_buf
FontManagerDrawTextProc* fontDrawText = nullptr;

// 0x51E3BC text_height
FontManagerGetLineHeightProc* fontGetLineHeight = nullptr;

// 0x51E3C0 text_width
FontManagerGetStringWidthProc* fontGetStringWidth = nullptr;

// 0x51E3C4 text_char_width
FontManagerGetCharacterWidthProc* fontGetCharacterWidth = nullptr;

// 0x51E3C8 text_mono_width
FontManagerGetMonospacedStringWidthProc* fontGetMonospacedStringWidth = nullptr;

// 0x51E3CC text_spacing
FontManagerGetLetterSpacingProc* fontGetLetterSpacing = nullptr;

// 0x51E3D0 text_size
FontManagerGetBufferSizeProc* fontGetBufferSize = nullptr;

// 0x51E3D4 text_max
FontManagerGetMonospacedCharacterWidth* fontGetMonospacedCharacterWidth = nullptr;

FontManagerDecodeCharacterProc* fontDecodeCharacter = nullptr;

// 0x6ADB08 font
static TextFontDescriptor gTextFontDescriptors[TEXT_FONT_MAX];

// 0x6ADBD0 font_managers
static FontManager gFontManagers[FONT_MANAGER_MAX];

// 0x6ADD88 curr_font
static TextFontDescriptor* gCurrentTextFontDescriptor;

// 0x4D555C GNW_text_init
int textFontsInit()
{
    int currentFont = -1;

    FontManager fontManager;
    memcpy(&fontManager, &gTextFontManager, sizeof(fontManager));

    for (int font = 0; font < TEXT_FONT_MAX; font++) {
        if (textFontLoad(font) == -1) {
            gTextFontDescriptors[font].glyphCount = 0;
        } else {
            if (currentFont == -1) {
                currentFont = font;
            }
        }
    }

    if (currentFont == -1) {
        return -1;
    }

    if (fontManagerAdd(&fontManager) == -1) {
        return -1;
    }

    fontSetCurrent(currentFont);

    return 0;
}

// 0x4D55CC GNW_text_exit
void textFontsExit()
{
    monochromeFontScalerClearCache();

    for (int index = 0; index < TEXT_FONT_MAX; index++) {
        TextFontDescriptor* textFontDescriptor = &(gTextFontDescriptors[index]);
        if (textFontDescriptor->glyphCount != 0) {
            internal_free(textFontDescriptor->glyphs);
            internal_free(textFontDescriptor->data);
        }
    }
}

// 0x4D55FC load_font
int textFontLoad(int font)
{
    if (font < 0 || font >= TEXT_FONT_MAX) {
        return -1;
    }

    int rc = -1;
    int fileSize = 0;
    int dataSize = 0;
    int glyphsPtr = 0;
    int dataPtr = 0;
    long long glyphTableSize = 0;

    TextFontDescriptor* textFontDescriptor = &(gTextFontDescriptors[font]);
    textFontDescriptor->data = nullptr;
    textFontDescriptor->glyphs = nullptr;
    textFontDescriptor->dataSize = 0;
    textFontDescriptor->maxGlyphWidth = 0;

    File* stream = nullptr;
    char path[COMPAT_MAX_PATH];

    // Try set language path first
    snprintf(path, sizeof(path), "text/%s/font%d.fon", settings.system.language.c_str(), font);
    stream = fileOpen(path, "rb");

    // Fallback to English if needed
    if (stream == nullptr && compat_stricmp(settings.system.language.c_str(), ENGLISH) != 0) {
        snprintf(path, sizeof(path), "text/%s/font%d.fon", ENGLISH, font);
        stream = fileOpen(path, "rb");
    }

    // fallback to original path
    if (stream == nullptr) {
        snprintf(path, sizeof(path), "font%d.fon", font);
        stream = fileOpen(path, "rb");
        if (stream == nullptr) {
            goto out;
        }
    }

    fileSize = fileGetSize(stream);
    if (fileSize < 20) {
        goto out;
    }

    // NOTE: Original code reads entire descriptor in one go. This does not work
    // in x64 because of the two pointers.

    if (fileRead(&(textFontDescriptor->glyphCount), 4, 1, stream) != 1) goto out;
    if (fileRead(&(textFontDescriptor->lineHeight), 4, 1, stream) != 1) goto out;
    if (fileRead(&(textFontDescriptor->letterSpacing), 4, 1, stream) != 1) goto out;

    if (fileRead(&glyphsPtr, 4, 1, stream) != 1) goto out;

    if (fileRead(&dataPtr, 4, 1, stream) != 1) goto out;

    if (textFontDescriptor->glyphCount <= 0
        || textFontDescriptor->glyphCount > 65536
        || textFontDescriptor->lineHeight <= 0
        || textFontDescriptor->lineHeight > 4096
        || textFontDescriptor->letterSpacing < 0
        || textFontDescriptor->letterSpacing > 4096) {
        goto out;
    }

    glyphTableSize = static_cast<long long>(textFontDescriptor->glyphCount) * sizeof(TextFontGlyph);
    if (20 + glyphTableSize > fileSize) {
        goto out;
    }

    textFontDescriptor->glyphs = (TextFontGlyph*)internal_malloc(textFontDescriptor->glyphCount * sizeof(TextFontGlyph));
    if (textFontDescriptor->glyphs == nullptr) {
        goto out;
    }

    if (fileRead(textFontDescriptor->glyphs, sizeof(TextFontGlyph), textFontDescriptor->glyphCount, stream) != textFontDescriptor->glyphCount) {
        goto out;
    }

    dataSize = fileSize - 20 - static_cast<int>(glyphTableSize);
    if (dataSize <= 0) {
        goto out;
    }

    for (int index = 0; index < textFontDescriptor->glyphCount; index++) {
        TextFontGlyph* glyph = &(textFontDescriptor->glyphs[index]);
        if (glyph->width < 0 || glyph->width > 4096 || glyph->dataOffset < 0) {
            goto out;
        }

        long long glyphSize = static_cast<long long>(textFontDescriptor->lineHeight) * ((glyph->width + 7) >> 3);
        if (glyph->dataOffset + glyphSize > dataSize) {
            goto out;
        }

        textFontDescriptor->maxGlyphWidth = std::max(textFontDescriptor->maxGlyphWidth, glyph->width);
    }

    textFontDescriptor->data = (unsigned char*)internal_malloc(dataSize);
    if (textFontDescriptor->data == nullptr) {
        goto out;
    }

    if (fileRead(textFontDescriptor->data, 1, dataSize, stream) != dataSize) {
        goto out;
    }

    textFontDescriptor->dataSize = dataSize;

    rc = 0;

out:

    if (rc != 0) {
        if (textFontDescriptor->data != nullptr) {
            internal_free(textFontDescriptor->data);
            textFontDescriptor->data = nullptr;
        }

        if (textFontDescriptor->glyphs != nullptr) {
            internal_free(textFontDescriptor->glyphs);
            textFontDescriptor->glyphs = nullptr;
        }

        textFontDescriptor->dataSize = 0;
        textFontDescriptor->maxGlyphWidth = 0;
        textFontDescriptor->glyphCount = 0;
        textFontDescriptor->lineHeight = 0;
        textFontDescriptor->letterSpacing = 0;
    }

    if (stream != nullptr) {
        fileClose(stream);
    }

    return rc;
}

int textFontDecodeDbcsCharacter(const char* string, int* length)
{
    if (length != nullptr) {
        *length = 0;
    }

    if (string == nullptr || *string == '\0') {
        return 0;
    }

    unsigned char lead = static_cast<unsigned char>(string[0]);
    if (length != nullptr) {
        *length = 1;
    }

    if (!textFontUsesDbcs()) {
        return lead;
    }

    if (lead < 0x81 || lead > 0xFE || string[1] == '\0') {
        return lead;
    }

    unsigned char trail = static_cast<unsigned char>(string[1]);
    if (trail < 0x40 || trail > 0xFE || trail == 0x7F) {
        return lead;
    }

    int ch = (lead << 8) | trail;
    if (length != nullptr) {
        *length = 2;
    }

    return ch;
}

bool textFontGetDbcsGlyph(int ch, TextFontGlyphView* glyphView)
{
    if (ch <= 0xFF) {
        return false;
    }

    for (int font = 0; font < TEXT_FONT_MAX; font++) {
        TextFontDescriptor* fontDescriptor = &(gTextFontDescriptors[font]);
        if (fontDescriptor->glyphCount <= 256) {
            continue;
        }

        TextFontGlyphView view;
        if (ch < fontDescriptor->glyphCount
            && textFontGetGlyphView(fontDescriptor, ch, &view)
            && view.width > 0) {
            if (glyphView != nullptr) {
                *glyphView = view;
            }
            return true;
        }

        // The supplied CJK bitmap fonts commonly leave the full-width space
        // empty. It still needs a full glyph advance to keep layout intact.
        if (ch == 0xA1A1) {
            if (glyphView != nullptr) {
                glyphView->width = fontDescriptor->lineHeight;
                glyphView->height = fontDescriptor->lineHeight;
                glyphView->rowBytes = 0;
                glyphView->data = nullptr;
            }
            return true;
        }

        // Some community fonts omit a handful of valid code points. Keep the
        // byte stream synchronized and use readable in-font fallbacks instead
        // of splitting the pair into two extended-ASCII characters.
        int fallback = ch == 0xA1A2 ? 0xA3AC : 0xA1F5;
        if (fallback < fontDescriptor->glyphCount
            && textFontGetGlyphView(fontDescriptor, fallback, &view)
            && view.width > 0) {
            if (glyphView != nullptr) {
                *glyphView = view;
            }
            return true;
        }
    }

    return false;
}

bool textFontHasDbcsGlyphs()
{
    for (int font = 0; font < TEXT_FONT_MAX; font++) {
        TextFontDescriptor* fontDescriptor = &(gTextFontDescriptors[font]);
        if (fontDescriptor->data != nullptr && fontDescriptor->glyphCount > 256) {
            return true;
        }
    }

    return false;
}

// 0x4D5780 text_add_manager
int fontManagerAdd(FontManager* fontManager)
{
    if (fontManager == nullptr) {
        return -1;
    }

    if (gFontManagersCount >= FONT_MANAGER_MAX) {
        return -1;
    }

    // Check if a font manager exists for any font in the specified range.
    for (int index = fontManager->minFont; index < fontManager->maxFont; index++) {
        FontManager* existingFontManager;
        if (fontManagerFind(index, &existingFontManager)) {
            return -1;
        }
    }

    memcpy(&(gFontManagers[gFontManagersCount]), fontManager, sizeof(*fontManager));
    gFontManagersCount++;

    return 0;
}

// 0x4D58AC GNW_text_font
static void textFontSetCurrentImpl(int font)
{
    if (font >= TEXT_FONT_MAX) {
        return;
    }

    TextFontDescriptor* textFontDescriptor = &(gTextFontDescriptors[font]);
    if (textFontDescriptor->glyphCount == 0) {
        return;
    }

    gCurrentTextFontDescriptor = textFontDescriptor;
}

// 0x4D58D4 text_curr
int fontGetCurrent()
{
    return gCurrentFont;
}

// 0x4D58DC text_font
void fontSetCurrent(int font)
{
    FontManager* fontManager;

    if (fontManagerFind(font, &fontManager)) {
        fontDrawText = fontManager->drawTextProc;
        fontGetLineHeight = fontManager->getLineHeightProc;
        fontGetStringWidth = fontManager->getStringWidthProc;
        fontGetCharacterWidth = fontManager->getCharacterWidthProc;
        fontGetMonospacedStringWidth = fontManager->getMonospacedStringWidthProc;
        fontGetLetterSpacing = fontManager->getLetterSpacingProc;
        fontGetBufferSize = fontManager->getBufferSizeProc;
        fontGetMonospacedCharacterWidth = fontManager->getMonospacedCharacterWidthProc;
        fontDecodeCharacter = fontManager->decodeCharacterProc;

        gCurrentFont = font;

        fontManager->setCurrentProc(font);
    }
}

// 0x4D595C text_font_exists
static bool fontManagerFind(int font, FontManager** fontManagerPtr)
{
    for (int index = 0; index < gFontManagersCount; index++) {
        FontManager* fontManager = &(gFontManagers[index]);
        if (font >= fontManager->minFont && font <= fontManager->maxFont) {
            *fontManagerPtr = fontManager;
            return true;
        }
    }

    return false;
}

static bool textFontGetGlyphView(const TextFontDescriptor* fontDescriptor, int ch, TextFontGlyphView* glyphView)
{
    if (fontDescriptor == nullptr
        || fontDescriptor->data == nullptr
        || ch < 0
        || ch >= fontDescriptor->glyphCount) {
        return false;
    }

    const TextFontGlyph* glyph = &(fontDescriptor->glyphs[ch]);
    int rowBytes = (glyph->width + 7) >> 3;
    long long glyphEnd = static_cast<long long>(glyph->dataOffset) + static_cast<long long>(rowBytes) * fontDescriptor->lineHeight;
    if (glyph->dataOffset < 0 || glyphEnd > fontDescriptor->dataSize) {
        return false;
    }

    if (glyphView != nullptr) {
        glyphView->width = glyph->width;
        glyphView->height = fontDescriptor->lineHeight;
        glyphView->rowBytes = rowBytes;
        glyphView->data = fontDescriptor->data + glyph->dataOffset;
    }

    return true;
}

static bool textFontUsesDbcs()
{
    return compat_stricmp(settings.system.language.c_str(), "chs") == 0;
}

static bool textFontGetCurrentGlyphView(int ch, TextFontGlyphView* glyphView)
{
    TextFontGlyphView view;
    if (textFontGetGlyphView(gCurrentTextFontDescriptor, ch, &view)
        && (ch <= 0xFF || view.width > 0)) {
        if (glyphView != nullptr) {
            *glyphView = view;
        }
        return true;
    }

    return textFontGetDbcsGlyph(ch, glyphView);
}

static int textFontGetScaledGlyphWidth(const TextFontGlyphView& glyphView)
{
    if (glyphView.width <= 0 || glyphView.height <= 0) {
        return 0;
    }

    return std::max(1, (glyphView.width * gCurrentTextFontDescriptor->lineHeight + glyphView.height / 2) / glyphView.height);
}

static void textFontDrawGlyph(unsigned char* buf, int pitch, int color, unsigned char* palette, bool isDbcs, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight)
{
    if (glyphView.data == nullptr
        || glyphView.width <= 0
        || glyphView.height <= 0
        || targetWidth <= 0
        || targetHeight <= 0) {
        return;
    }

    MonochromeGlyphCoverageView coverageView;
    if (isDbcs
        && palette != nullptr
        && monochromeFontGetDownscaledGlyphCoverage(glyphView.data,
            glyphView.width,
            glyphView.height,
            glyphView.rowBytes,
            targetWidth,
            targetHeight,
            &coverageView)) {
        for (int y = 0; y < targetHeight; y++) {
            unsigned char* destination = buf + y * pitch;
            const unsigned char* coverage = coverageView.coverage + y * targetWidth;
            for (int x = 0; x < targetWidth; x++) {
                unsigned char level = coverage[x];
                if (level == 7) {
                    destination[x] = color & 0xFF;
                } else if (level != 0) {
                    destination[x] = palette[(level << 8) + destination[x]];
                }
            }
        }
        return;
    }

    for (int y = 0; y < targetHeight; y++) {
        int sourceY = y * glyphView.height / targetHeight;
        const unsigned char* sourceRow = glyphView.data + sourceY * glyphView.rowBytes;
        unsigned char* destination = buf + y * pitch;

        for (int x = 0; x < targetWidth; x++) {
            int sourceX = x * glyphView.width / targetWidth;
            if ((sourceRow[sourceX >> 3] & (0x80 >> (sourceX & 7))) != 0) {
                destination[x] = color & 0xFF;
            }
        }
    }
}

// 0x4D59B0 GNW_text_to_buf
static void textFontDrawImpl(unsigned char* buf, const char* string, int length, int pitch, int color)
{
    if ((color & DRAW_TEXT_FLAG_SHADOWED) != 0) {
        color &= ~DRAW_TEXT_FLAG_SHADOWED;
        fontDrawText(buf + pitch + 1, string, length, pitch, COLOR_BLACK);
    }

    unsigned char* palette = nullptr;

    int monospacedCharacterWidth;
    if ((color & DRAW_TEXT_FLAG_MONOSPACED) != 0) {
        monospacedCharacterWidth = fontGetMonospacedCharacterWidth();
    }

    unsigned char* ptr = buf;
    while (*string != '\0') {
        int characterLength;
        int ch = textFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        TextFontGlyphView glyphView;
        if (!textFontGetCurrentGlyphView(ch, &glyphView)) {
            continue;
        }

        int characterWidth = textFontGetScaledGlyphWidth(glyphView);
        unsigned char* end;
        if ((color & DRAW_TEXT_FLAG_MONOSPACED) != 0) {
            end = ptr + monospacedCharacterWidth;
            ptr += (monospacedCharacterWidth - gCurrentTextFontDescriptor->letterSpacing - characterWidth) / 2;
        } else {
            end = ptr + characterWidth + gCurrentTextFontDescriptor->letterSpacing;
        }

        if (end - buf > length) {
            break;
        }

        bool isDbcs = ch > 0xFF;
        if (isDbcs
            && palette == nullptr
            && (characterWidth < glyphView.width || gCurrentTextFontDescriptor->lineHeight < glyphView.height)) {
            palette = _getColorBlendTable(color & 0xFF);
        }

        textFontDrawGlyph(ptr,
            pitch,
            color,
            palette,
            isDbcs,
            glyphView,
            characterWidth,
            gCurrentTextFontDescriptor->lineHeight);

        ptr = end;
    }

    if ((color & DRAW_TEXT_FLAG_UNDERLINED) != 0) {
        // TODO: Probably additional -1 present, check.
        int length = ptr - buf;
        unsigned char* underlinePtr = buf + pitch * (gCurrentTextFontDescriptor->lineHeight - 1);
        for (int pix = 0; pix < length; pix++) {
            *underlinePtr++ = color & 0xFF;
        }
    }

    if (palette != nullptr) {
        _freeColorBlendTable(color & 0xFF);
    }
}

// 0x4D5B54 GNW_text_height
static int textFontGetLineHeightImpl()
{
    return gCurrentTextFontDescriptor->lineHeight;
}

// 0x4D5B60 GNW_text_width
static int textFontGeStringWidthImpl(const char* string)
{
    int width = 0;

    while (*string != '\0') {
        int characterLength;
        int ch = textFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        TextFontGlyphView glyphView;
        if (textFontGetCurrentGlyphView(ch, &glyphView)) {
            width += gCurrentTextFontDescriptor->letterSpacing + textFontGetScaledGlyphWidth(glyphView);
        }
    }

    return width;
}

// 0x4D5BA4 GNW_text_char_width
static int textFontGetCharacterWidthImpl(int ch)
{
    TextFontGlyphView glyphView;
    if (!textFontGetCurrentGlyphView(ch, &glyphView)) {
        return 0;
    }

    return textFontGetScaledGlyphWidth(glyphView);
}

// 0x4D5BB8 GNW_text_mono_width
static int textFontGetMonospacedStringWidthImpl(const char* string)
{
    int characters = 0;

    while (*string != '\0') {
        int characterLength;
        textFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }

        string += characterLength;
        characters++;
    }

    return fontGetMonospacedCharacterWidth() * characters;
}

// 0x4D5BD8 GNW_text_spacing
static int textFontGetLetterSpacingImpl()
{
    return gCurrentTextFontDescriptor->letterSpacing;
}

// 0x4D5BE4 GNW_text_size
static int textFontGetBufferSizeImpl(const char* string)
{
    return fontGetStringWidth(string) * fontGetLineHeight();
}

// 0x4D5BF8 GNW_text_max
static int textFontGetMonospacedCharacterWidthImpl()
{
    int width = gCurrentTextFontDescriptor->maxGlyphWidth;

    if (gCurrentTextFontDescriptor->glyphCount <= 256) {
        for (int font = 0; font < TEXT_FONT_MAX; font++) {
            TextFontDescriptor* fontDescriptor = &(gTextFontDescriptors[font]);
            if (fontDescriptor->glyphCount <= 256) {
                continue;
            }

            TextFontGlyphView glyphView;
            glyphView.width = fontDescriptor->maxGlyphWidth;
            glyphView.height = fontDescriptor->lineHeight;
            width = std::max(width, textFontGetScaledGlyphWidth(glyphView));
        }
    }

    return width + gCurrentTextFontDescriptor->letterSpacing;
}

static int textFontDecodeCharacterImpl(const char* string, int* length)
{
    return textFontDecodeDbcsCharacter(string, length);
}

void fontDrawText2D(const Buffer2D& dest, int xPos, int yPos, const char* string, int length, int color)
{
    assert(dest.data != nullptr && dest.width > 0 && dest.height > 0);
    assert(xPos >= 0 && xPos < dest.width && yPos >= 0 && yPos < dest.height && length >= 0 && length < dest.width - xPos);

    xPos = std::clamp(xPos, 0, dest.width - 1);
    yPos = std::clamp(yPos, 0, dest.height - 1);
    length = std::clamp(length, 0, dest.width - xPos);
    fontDrawText(dest.data + dest.width * yPos + xPos, string, length, dest.width, color);
}

} // namespace fallout
