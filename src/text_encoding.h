#ifndef TEXT_ENCODING_H
#define TEXT_ENCODING_H

#include <stddef.h>

#include <string>

namespace fallout {

// Loads a localized display-text file and normalizes Unicode input to the
// legacy encoding expected by the active bitmap font. Existing legacy text is
// preserved byte-for-byte.
bool textEncodingLoadFile(const char* path, std::string* output);

// Normalizes an in-memory text buffer using the active language and
// [system] text_encoding setting.
bool textEncodingNormalize(const char* data, size_t size, std::string* output);

// Returns the encoded byte length of the next character in normalized text.
size_t textEncodingCharacterLength(const char* text, size_t remaining);

// Counts display characters in normalized text rather than encoded bytes.
int textEncodingCharacterCount(const char* text);

} // namespace fallout

#endif /* TEXT_ENCODING_H */
