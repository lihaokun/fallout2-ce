#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include "text_font.h"

namespace fallout {

extern FontManager gModernFontManager;

int interfaceFontsInit();
void interfaceFontsExit();
// Scaled interface font drawing supports plain color text plus DRAW_TEXT_FLAG_SHADOWED.
// Unsupported flags like DRAW_TEXT_FLAG_MONOSPACED and DRAW_TEXT_FLAG_UNDERLINED are ignored with a debug log.
void interfaceFontDrawTextScaled2D(const Buffer2D& dest, int x, int y, const char* string, int color, float scale);
int interfaceFontGetStringWidthScaled(const char* string, int color, float scale);

int interfaceFontSetCjkMinimumHeight(int height);

class ScopedCjkInterfaceFontHeight {
public:
    explicit ScopedCjkInterfaceFontHeight(int height)
        : _previousHeight(interfaceFontSetCjkMinimumHeight(height))
    {
    }

    ~ScopedCjkInterfaceFontHeight()
    {
        interfaceFontSetCjkMinimumHeight(_previousHeight);
    }

    ScopedCjkInterfaceFontHeight(const ScopedCjkInterfaceFontHeight&) = delete;
    ScopedCjkInterfaceFontHeight& operator=(const ScopedCjkInterfaceFontHeight&) = delete;

private:
    int _previousHeight;
};

} // namespace fallout

#endif /* FONT_MANAGER_H */
