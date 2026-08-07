#include "word_wrap.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "text_font.h"

namespace fallout {

// 0x4BC6F0 word_wrap
int wordWrap(const char* string, int width, short* breakpoints, short* breakpointsLengthPtr)
{
    breakpoints[0] = 0;
    *breakpointsLengthPtr = 1;

    for (int index = 1; index < WORD_WRAP_MAX_COUNT; index++) {
        breakpoints[index] = -1;
    }

    if (fontGetMonospacedCharacterWidth() > width) {
        return -1;
    }

    if (fontGetStringWidth(string) < width) {
        breakpoints[*breakpointsLengthPtr] = (short)strlen(string);
        *breakpointsLengthPtr += 1;
        return 0;
    }

    int gap = fontGetLetterSpacing();

    int accum = 0;
    const char* lineStart = string;
    const char* previousBreak = nullptr;
    const char* pch = string;
    while (*pch != '\0') {
        int characterLength;
        int ch = fontDecodeCharacter(pch, &characterLength);
        if (characterLength <= 0) {
            return -1;
        }

        int characterWidth = gap + fontGetCharacterWidth(ch);
        if (accum + characterWidth <= width) {
            accum += characterWidth;

            // NOTE: quests.txt #807 uses extended ascii.
            if (ch <= 0xFF && (isspace(ch) || ch == '-')) {
                previousBreak = pch + characterLength;
            }

            pch += characterLength;
            continue;
        }

        if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
            return -1;
        }

        const char* nextLine;
        if (previousBreak != nullptr && previousBreak > lineStart) {
            nextLine = previousBreak;
        } else {
            // The first glyph should have fit because of the monospaced width
            // check above. Keep this guard to avoid looping on malformed fonts.
            if (pch == lineStart) {
                return -1;
            }

            nextLine = pch;
        }

        breakpoints[*breakpointsLengthPtr] = nextLine - string;
        *breakpointsLengthPtr += 1;

        lineStart = nextLine;
        pch = nextLine;
        previousBreak = nullptr;
        accum = 0;
    }

    if (*breakpointsLengthPtr == WORD_WRAP_MAX_COUNT) {
        return -1;
    }

    breakpoints[*breakpointsLengthPtr] = pch - string;
    *breakpointsLengthPtr += 1;

    return 0;
}

} // namespace fallout
