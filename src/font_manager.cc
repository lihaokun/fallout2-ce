#include "font_manager.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "color.h"
#include "db.h"
#include "debug.h"
#include "memory_manager.h"
#include "settings.h"
#include "window_manager.h"

// The maximum number of interface fonts.
#define INTERFACE_FONT_MAX (16)

namespace fallout {

typedef struct InterfaceFontGlyph {
    short width;
    short height;
    int offset;
} InterfaceFontGlyph;

typedef struct InterfaceFontDescriptor {
    short maxHeight;
    short letterSpacing;
    short wordSpacing;
    short lineSpacing;
    short field_8;
    short field_A;
    InterfaceFontGlyph glyphs[256];
    unsigned char* data;
} InterfaceFontDescriptor;

static int interfaceFontLoad(int font);
static void interfaceFontSetCurrentImpl(int font);
static int interfaceFontGetLineHeightImpl();
static int interfaceFontGetStringWidthImpl(const char* string);
static int interfaceFontGetCharacterWidthImpl(int ch);
static int interfaceFontGetMonospacedStringWidthImpl(const char* string);
static int interfaceFontGetLetterSpacingImpl();
static int interfaceFontGetBufferSizeImpl(const char* string);
static int interfaceFontGetMonospacedCharacterWidthImpl();
static int interfaceFontDecodeCharacterImpl(const char* string, int* length);
static void interfaceFontDrawImpl(unsigned char* buf, const char* string, int length, int pitch, int color);
static void interfaceFontByteSwapUInt32(unsigned int* value);
static void interfaceFontByteSwapInt32(int* value);
static void interfaceFontByteSwapUInt16(unsigned short* value);
static void interfaceFontByteSwapInt16(short* value);
static void interfaceFontDrawScaledImpl(const Buffer2D& dest, int x, int y, const char* string, int color, float scale);
static int interfaceFontGetScaledWidthImpl(const char* string, int color, float scale);
static int interfaceFontGetDbcsGlyphWidth(const TextFontGlyphView& glyphView);
static void interfaceFontDrawDbcsGlyph(unsigned char* buf, int pitch, int color, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight);
static void interfaceFontDrawDbcsGlyphScaled2D(const Buffer2D& dest, int x, int y, int color, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight);

// 0x518680 gFMInit
static bool gInterfaceFontsInitialized = false;

// 0x518684 gNumFonts
static int gInterfaceFontsLength = 0;

// 0x518688 alias_mgr
FontManager gModernFontManager = {
    100,
    110,
    interfaceFontSetCurrentImpl,
    interfaceFontDrawImpl,
    interfaceFontGetLineHeightImpl,
    interfaceFontGetStringWidthImpl,
    interfaceFontGetCharacterWidthImpl,
    interfaceFontGetMonospacedStringWidthImpl,
    interfaceFontGetLetterSpacingImpl,
    interfaceFontGetBufferSizeImpl,
    interfaceFontGetMonospacedCharacterWidthImpl,
    interfaceFontDecodeCharacterImpl,
};

// 0x586838 gFontCache
static InterfaceFontDescriptor gInterfaceFontDescriptors[INTERFACE_FONT_MAX];

// 0x58E938 gCurrentFontNum
static int gCurrentInterfaceFont;

// 0x58E93C gCurrentFont
static InterfaceFontDescriptor* gCurrentInterfaceFontDescriptor;

// 0x441C80 FMInit
int interfaceFontsInit()
{
    int currentFont = -1;

    for (int font = 0; font < INTERFACE_FONT_MAX; font++) {
        if (interfaceFontLoad(font) == -1) {
            gInterfaceFontDescriptors[font].maxHeight = 0;
            gInterfaceFontDescriptors[font].data = nullptr;
        } else {
            ++gInterfaceFontsLength;

            if (currentFont == -1) {
                currentFont = font;
            }
        }
    }

    if (currentFont == -1) {
        return -1;
    }

    gInterfaceFontsInitialized = true;

    interfaceFontSetCurrentImpl(currentFont + 100);

    return 0;
}

// 0x441CEC FMExit
void interfaceFontsExit()
{
    for (int font = 0; font < INTERFACE_FONT_MAX; font++) {
        if (gInterfaceFontDescriptors[font].data != nullptr) {
            internal_free_safe(gInterfaceFontDescriptors[font].data, __FILE__, __LINE__); // FONTMGR.C, 124
        }
    }
}

void interfaceFontDrawTextScaled2D(const Buffer2D& dest, int x, int y, const char* string, int color, float scale)
{
    if (!gInterfaceFontsInitialized || dest.data == nullptr || string == nullptr || *string == '\0') {
        return;
    }

    interfaceFontDrawScaledImpl(dest, x, y, string, color, std::max(scale, 0.01f));
}

int interfaceFontGetStringWidthScaled(const char* string, int color, float scale)
{
    if (!gInterfaceFontsInitialized || string == nullptr || *string == '\0') {
        return 0;
    }

    return interfaceFontGetScaledWidthImpl(string, color, std::max(scale, 0.01f));
}

// 0x441D20 FMLoadFont
static int interfaceFontLoad(int font_index)
{
    InterfaceFontDescriptor* fontDescriptor = &(gInterfaceFontDescriptors[font_index]);

    char path[56];
    File* stream = nullptr;

    // Try set language path first
    snprintf(path, sizeof(path), "text/%s/font%d.aaf", settings.system.language.c_str(), font_index);
    stream = fileOpen(path, "rb");

    // Fallback to English if needed
    if (stream == nullptr && compat_stricmp(settings.system.language.c_str(), ENGLISH) != 0) {
        snprintf(path, sizeof(path), "text/%s/font%d.aaf", ENGLISH, font_index);
        stream = fileOpen(path, "rb");
    }

    // Fallback to original path
    if (stream == nullptr) {
        snprintf(path, sizeof(path), "font%d.aaf", font_index);
        stream = fileOpen(path, "rb");
        if (stream == nullptr) {
            return -1;
        }
    }

    int fileSize = fileGetSize(stream);

    int sig;
    if (fileRead(&sig, 4, 1, stream) != 1) {
        fileClose(stream);
        return -1;
    }

    interfaceFontByteSwapInt32(&sig);
    if (sig != 0x41414646) {
        fileClose(stream);
        return -1;
    }

    if (fileRead(&(fontDescriptor->maxHeight), 2, 1, stream) != 1) {
        fileClose(stream);
        return -1;
    }
    interfaceFontByteSwapInt16(&(fontDescriptor->maxHeight));

    if (fileRead(&(fontDescriptor->letterSpacing), 2, 1, stream) != 1) {
        fileClose(stream);
        return -1;
    }
    interfaceFontByteSwapInt16(&(fontDescriptor->letterSpacing));

    if (fileRead(&(fontDescriptor->wordSpacing), 2, 1, stream) != 1) {
        fileClose(stream);
        return -1;
    }
    interfaceFontByteSwapInt16(&(fontDescriptor->wordSpacing));

    if (fileRead(&(fontDescriptor->lineSpacing), 2, 1, stream) != 1) {
        fileClose(stream);
        return -1;
    }
    interfaceFontByteSwapInt16(&(fontDescriptor->lineSpacing));

    for (int index = 0; index < 256; index++) {
        InterfaceFontGlyph* glyph = &(fontDescriptor->glyphs[index]);

        if (fileRead(&(glyph->width), 2, 1, stream) != 1) {
            fileClose(stream);
            return -1;
        }
        interfaceFontByteSwapInt16(&(glyph->width));

        if (fileRead(&(glyph->height), 2, 1, stream) != 1) {
            fileClose(stream);
            return -1;
        }
        interfaceFontByteSwapInt16(&(glyph->height));

        if (fileRead(&(glyph->offset), 4, 1, stream) != 1) {
            fileClose(stream);
            return -1;
        }
        interfaceFontByteSwapInt32(&(glyph->offset));
    }

    int glyphDataSize = fileSize - 2060;

    fontDescriptor->data = (unsigned char*)internal_malloc_safe(glyphDataSize, __FILE__, __LINE__); // FONTMGR.C, 259
    if (fontDescriptor->data == nullptr) {
        fileClose(stream);
        return -1;
    }

    if (fileRead(fontDescriptor->data, glyphDataSize, 1, stream) != 1) {
        internal_free_safe(fontDescriptor->data, __FILE__, __LINE__); // FONTMGR.C, 268
        fileClose(stream);
        return -1;
    }

    fileClose(stream);
    return 0;
}

// 0x442120 FMtext_font
static void interfaceFontSetCurrentImpl(int font)
{
    if (!gInterfaceFontsInitialized) {
        return;
    }

    font -= 100;

    if (gInterfaceFontDescriptors[font].data != nullptr) {
        gCurrentInterfaceFont = font;
        gCurrentInterfaceFontDescriptor = &(gInterfaceFontDescriptors[font]);
    }
}

// 0x442168 FMtext_height
static int interfaceFontGetLineHeightImpl()
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    return gCurrentInterfaceFontDescriptor->lineSpacing + gCurrentInterfaceFontDescriptor->maxHeight;
}

// 0x442188 FMtext_width
static int interfaceFontGetStringWidthImpl(const char* string)
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    int stringWidth = 0;

    while (*string != '\0') {
        int characterLength;
        int ch = interfaceFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        stringWidth += interfaceFontGetCharacterWidthImpl(ch) + gCurrentInterfaceFontDescriptor->letterSpacing;
    }

    return stringWidth;
}

// 0x4421DC FMtext_char_width
static int interfaceFontGetCharacterWidthImpl(int ch)
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    if (ch > 0xFF) {
        TextFontGlyphView glyphView;
        if (textFontGetDbcsGlyph(ch, &glyphView)) {
            return interfaceFontGetDbcsGlyphWidth(glyphView);
        }

        return 0;
    }

    if (ch < 0) {
        return 0;
    }

    if (ch == ' ') {
        return gCurrentInterfaceFontDescriptor->wordSpacing;
    }

    return gCurrentInterfaceFontDescriptor->glyphs[ch].width;
}

// 0x442210 FMtext_mono_width
static int interfaceFontGetMonospacedStringWidthImpl(const char* str)
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    int characters = 0;
    while (*str != '\0') {
        int characterLength;
        interfaceFontDecodeCharacterImpl(str, &characterLength);
        if (characterLength == 0) {
            break;
        }

        str += characterLength;
        characters++;
    }

    return interfaceFontGetMonospacedCharacterWidthImpl() * characters;
}

// 0x442240 FMtext_spacing
static int interfaceFontGetLetterSpacingImpl()
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    return gCurrentInterfaceFontDescriptor->letterSpacing;
}

// 0x442258 FMtext_size
static int interfaceFontGetBufferSizeImpl(const char* str)
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    return interfaceFontGetStringWidthImpl(str) * interfaceFontGetLineHeightImpl();
}

// 0x442278 FMtext_max
static int interfaceFontGetMonospacedCharacterWidthImpl()
{
    if (!gInterfaceFontsInitialized) {
        return 0;
    }

    int spacing;
    if (gCurrentInterfaceFontDescriptor->wordSpacing <= gCurrentInterfaceFontDescriptor->field_8) {
        spacing = gCurrentInterfaceFontDescriptor->lineSpacing;
    } else {
        spacing = gCurrentInterfaceFontDescriptor->letterSpacing;
    }

    return spacing + gCurrentInterfaceFontDescriptor->maxHeight;
}

// 0x4422B4 FMtext_to_buf
static void interfaceFontDrawImpl(unsigned char* buf, const char* string, int length, int pitch, int color)
{
    if (!gInterfaceFontsInitialized) {
        return;
    }

    if ((color & DRAW_TEXT_FLAG_SHADOWED) != 0) {
        color &= ~DRAW_TEXT_FLAG_SHADOWED;
        // NOTE: Other font options preserved. This is different from text font
        // shadows.
        interfaceFontDrawImpl(buf + pitch + 1, string, length, pitch, (color & ~0xFF) | COLOR_BLACK);
    }

    unsigned char* palette = _getColorBlendTable(color & 0xFF);

    int monospacedCharacterWidth;
    if ((color & DRAW_TEXT_FLAG_MONOSPACED) != 0) {
        // NOTE: Uninline.
        monospacedCharacterWidth = interfaceFontGetMonospacedCharacterWidthImpl();
    }

    unsigned char* ptr = buf;
    while (*string != '\0') {
        int characterLength;
        int ch = interfaceFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        int characterWidth = interfaceFontGetCharacterWidthImpl(ch);

        unsigned char* end;
        if ((color & DRAW_TEXT_FLAG_MONOSPACED) != 0) {
            end = ptr + monospacedCharacterWidth;
            ptr += (monospacedCharacterWidth - characterWidth - gCurrentInterfaceFontDescriptor->letterSpacing) / 2;
        } else {
            end = ptr + characterWidth + gCurrentInterfaceFontDescriptor->letterSpacing;
        }

        if (end - buf > length) {
            break;
        }

        if (ch > 0xFF) {
            TextFontGlyphView glyphView;
            if (textFontGetDbcsGlyph(ch, &glyphView)) {
                interfaceFontDrawDbcsGlyph(ptr,
                    pitch,
                    color,
                    glyphView,
                    characterWidth,
                    gCurrentInterfaceFontDescriptor->maxHeight);
            }
        } else {
            InterfaceFontGlyph* glyph = &(gCurrentInterfaceFontDescriptor->glyphs[ch]);
            unsigned char* glyphDataPtr = gCurrentInterfaceFontDescriptor->data + glyph->offset;

            // Skip blank pixels (difference between font's line height and glyph height).
            ptr += (gCurrentInterfaceFontDescriptor->maxHeight - glyph->height) * pitch;

            for (int y = 0; y < glyph->height; y++) {
                for (int x = 0; x < glyph->width; x++) {
                    unsigned char byte = *glyphDataPtr++;

                    *ptr++ = palette[(byte << 8) + *ptr];
                }

                ptr += pitch - glyph->width;
            }
        }

        ptr = end;
    }

    if ((color & DRAW_TEXT_FLAG_UNDERLINED) != 0) {
        int length = ptr - buf;
        unsigned char* underlinePtr = buf + pitch * (gCurrentInterfaceFontDescriptor->maxHeight - 1);
        for (int index = 0; index < length; index++) {
            *underlinePtr++ = color & 0xFF;
        }
    }

    _freeColorBlendTable(color & 0xFF);
}

static void interfaceFontDrawScaledImpl(const Buffer2D& dest, int x, int y, const char* string, int color, float scale)
{
    if ((color & DRAW_TEXT_FLAG_SHADOWED) != 0) {
        color &= ~DRAW_TEXT_FLAG_SHADOWED;
        interfaceFontDrawScaledImpl(dest, x + 1, y + 1, string, (color & ~0xFF) | COLOR_BLACK, scale);
    }

    if ((color & (DRAW_TEXT_FLAG_MONOSPACED | DRAW_TEXT_FLAG_UNDERLINED)) != 0) {
        debugPrint("FONTMGR: scaled interface font draw ignores unsupported flags\n");
    }

    unsigned char* palette = _getColorBlendTable(color & 0xFF);

    float cursorX = static_cast<float>(x);
    while (*string != '\0') {
        int characterLength;
        int ch = interfaceFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        int characterWidth = interfaceFontGetCharacterWidthImpl(ch);

        int advance = characterWidth + gCurrentInterfaceFontDescriptor->letterSpacing;
        int glyphX = static_cast<int>(lround(cursorX));

        if (ch > 0xFF) {
            TextFontGlyphView glyphView;
            if (textFontGetDbcsGlyph(ch, &glyphView)) {
                int scaledGlyphWidth = std::max(1, static_cast<int>(lround(characterWidth * scale)));
                int scaledGlyphHeight = std::max(1, static_cast<int>(lround(gCurrentInterfaceFontDescriptor->maxHeight * scale)));
                interfaceFontDrawDbcsGlyphScaled2D(dest,
                    glyphX,
                    y,
                    color,
                    glyphView,
                    scaledGlyphWidth,
                    scaledGlyphHeight);
            }
        } else {
            InterfaceFontGlyph* glyph = &(gCurrentInterfaceFontDescriptor->glyphs[ch]);
            if (glyph->width > 0 && glyph->height > 0) {
                unsigned char* glyphData = gCurrentInterfaceFontDescriptor->data + glyph->offset;
                int scaledGlyphWidth = std::max(1, static_cast<int>(lround(glyph->width * scale)));
                int scaledGlyphHeight = std::max(1, static_cast<int>(lround(glyph->height * scale)));
                int glyphY = y + std::max(0, static_cast<int>(lround((gCurrentInterfaceFontDescriptor->maxHeight - glyph->height) * scale)));

                int destLeft = std::max(glyphX, 0);
                int destTop = std::max(glyphY, 0);
                int destRight = std::min(glyphX + scaledGlyphWidth, dest.width);
                int destBottom = std::min(glyphY + scaledGlyphHeight, dest.height);

                for (int destY = destTop; destY < destBottom; destY++) {
                    int scaledY = destY - glyphY;
                    int srcY = std::clamp(static_cast<int>(scaledY / scale), 0, glyph->height - 1);

                    for (int destX = destLeft; destX < destRight; destX++) {
                        int scaledX = destX - glyphX;
                        int srcX = std::clamp(static_cast<int>(scaledX / scale), 0, glyph->width - 1);
                        unsigned char byte = glyphData[srcY * glyph->width + srcX];
                        unsigned char* pixel = dest.data + destY * dest.width + destX;
                        *pixel = palette[(byte << 8) + *pixel];
                    }
                }
            }
        }

        cursorX += advance * scale;
        if (static_cast<int>(lround(cursorX)) >= dest.width) {
            break;
        }
    }

    _freeColorBlendTable(color & 0xFF);
}

static int interfaceFontGetScaledWidthImpl(const char* string, int color, float scale)
{
    if ((color & (DRAW_TEXT_FLAG_MONOSPACED | DRAW_TEXT_FLAG_UNDERLINED)) != 0) {
        debugPrint("FONTMGR: scaled interface font draw ignores unsupported flags\n");
    }

    float width = 0.0f;
    while (*string != '\0') {
        int characterLength;
        int ch = interfaceFontDecodeCharacterImpl(string, &characterLength);
        if (characterLength == 0) {
            break;
        }
        string += characterLength;

        int characterWidth = interfaceFontGetCharacterWidthImpl(ch);
        int advance = characterWidth + gCurrentInterfaceFontDescriptor->letterSpacing;
        width += advance * scale;
    }

    return std::max(0, static_cast<int>(lround(width)));
}

static int interfaceFontDecodeCharacterImpl(const char* string, int* length)
{
    return textFontDecodeDbcsCharacter(string, length);
}

static int interfaceFontGetDbcsGlyphWidth(const TextFontGlyphView& glyphView)
{
    if (glyphView.width <= 0 || glyphView.height <= 0) {
        return 0;
    }

    return std::max(1, (glyphView.width * gCurrentInterfaceFontDescriptor->maxHeight + glyphView.height / 2) / glyphView.height);
}

static void interfaceFontDrawDbcsGlyph(unsigned char* buf, int pitch, int color, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight)
{
    if (glyphView.data == nullptr
        || glyphView.width <= 0
        || glyphView.height <= 0
        || targetWidth <= 0
        || targetHeight <= 0) {
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

static void interfaceFontDrawDbcsGlyphScaled2D(const Buffer2D& dest, int x, int y, int color, const TextFontGlyphView& glyphView, int targetWidth, int targetHeight)
{
    if (dest.data == nullptr
        || glyphView.data == nullptr
        || glyphView.width <= 0
        || glyphView.height <= 0
        || targetWidth <= 0
        || targetHeight <= 0) {
        return;
    }

    int destinationLeft = std::max(x, 0);
    int destinationTop = std::max(y, 0);
    int destinationRight = std::min(x + targetWidth, dest.width);
    int destinationBottom = std::min(y + targetHeight, dest.height);

    for (int destinationY = destinationTop; destinationY < destinationBottom; destinationY++) {
        int sourceY = (destinationY - y) * glyphView.height / targetHeight;
        const unsigned char* sourceRow = glyphView.data + sourceY * glyphView.rowBytes;

        for (int destinationX = destinationLeft; destinationX < destinationRight; destinationX++) {
            int sourceX = (destinationX - x) * glyphView.width / targetWidth;
            if ((sourceRow[sourceX >> 3] & (0x80 >> (sourceX & 7))) != 0) {
                dest.data[destinationY * dest.width + destinationX] = color & 0xFF;
            }
        }
    }
}

// NOTE: Inlined.
//
// 0x442520 Swap4
static void interfaceFontByteSwapUInt32(unsigned int* value)
{
    unsigned int swapped = *value;
    unsigned short high = swapped >> 16;
    // NOTE: Uninline.
    interfaceFontByteSwapUInt16(&high);
    unsigned short low = swapped & 0xFFFF;
    // NOTE: Uninline.
    interfaceFontByteSwapUInt16(&low);
    *value = (low << 16) | high;
}

// NOTE: 0x442520 with different signature.
static void interfaceFontByteSwapInt32(int* value)
{
    interfaceFontByteSwapUInt32((unsigned int*)value);
}

// 0x442568 Swap2
static void interfaceFontByteSwapUInt16(unsigned short* value)
{
    unsigned short swapped = *value;
    swapped = (swapped >> 8) | (swapped << 8);
    *value = swapped;
}

// NOTE: 0x442568 with different signature.
static void interfaceFontByteSwapInt16(short* value)
{
    interfaceFontByteSwapUInt16((unsigned short*)value);
}

} // namespace fallout
