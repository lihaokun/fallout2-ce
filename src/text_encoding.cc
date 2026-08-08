#include "text_encoding.h"

#include <stdint.h>
#include <string.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "db.h"
#include "debug.h"
#include "platform_compat.h"
#include "settings.h"

namespace fallout {
namespace {

    constexpr unsigned char kUtf8Bom[] = { 0xEF, 0xBB, 0xBF };
    constexpr unsigned short kMissingGbkGlyph = 0xA1F5;

#include "text_encoding_gbk.inc"

    int base64Value(unsigned char ch)
    {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    }

    bool decodeBase64(const char* input, std::vector<unsigned char>* output)
    {
        if (input == nullptr || output == nullptr) return false;

        output->clear();
        uint32_t accumulator = 0;
        int bits = 0;
        bool sawPadding = false;

        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p != '\0'; p++) {
            unsigned char ch = *p;
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;

            if (ch == '=') {
                sawPadding = true;
                continue;
            }

            if (sawPadding) return false;

            int value = base64Value(ch);
            if (value < 0) return false;

            accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                output->push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
                if (bits == 0) {
                    accumulator = 0;
                } else {
                    accumulator &= (1U << bits) - 1;
                }
            }
        }

        return bits == 0 || accumulator == 0;
    }

    struct UnicodeToGbkMap {
        std::array<unsigned short, 65536> values = {};
        bool loaded = false;

        UnicodeToGbkMap()
        {
            std::vector<unsigned char> compressed;
            if (!decodeBase64(kUnicodeToGbkBase64, &compressed)
                || compressed.size() != kUnicodeToGbkCompressedSize) {
                debugPrint("Unable to decode embedded Unicode-to-GBK table.\n");
                return;
            }

            std::vector<unsigned char> raw(kUnicodeToGbkRawSize);
            uLongf rawSize = static_cast<uLongf>(raw.size());
            if (uncompress(raw.data(), &rawSize, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK
                || rawSize != kUnicodeToGbkRawSize
                || crc32(0, raw.data(), rawSize) != kUnicodeToGbkCrc32) {
                debugPrint("Unable to decompress embedded Unicode-to-GBK table.\n");
                return;
            }

            for (size_t index = 0; index < values.size(); index++) {
                values[index] = static_cast<unsigned short>(raw[index * 2]
                    | (static_cast<unsigned short>(raw[index * 2 + 1]) << 8));
            }

            loaded = true;
        }
    };

    const UnicodeToGbkMap& unicodeToGbkMap()
    {
        static const UnicodeToGbkMap map;
        return map;
    }

    bool decodeUtf8Character(const unsigned char* data, size_t size, size_t offset, uint32_t* codePoint, size_t* length)
    {
        if (data == nullptr || offset >= size || codePoint == nullptr || length == nullptr) return false;

        unsigned char ch0 = data[offset];
        if (ch0 <= 0x7F) {
            *codePoint = ch0;
            *length = 1;
            return true;
        }

        if (ch0 >= 0xC2 && ch0 <= 0xDF) {
            if (offset + 1 >= size) return false;
            unsigned char ch1 = data[offset + 1];
            if (ch1 < 0x80 || ch1 > 0xBF) return false;
            *codePoint = ((ch0 & 0x1F) << 6) | (ch1 & 0x3F);
            *length = 2;
            return true;
        }

        if (ch0 >= 0xE0 && ch0 <= 0xEF) {
            if (offset + 2 >= size) return false;
            unsigned char ch1 = data[offset + 1];
            unsigned char ch2 = data[offset + 2];
            if (ch2 < 0x80 || ch2 > 0xBF) return false;
            if (ch0 == 0xE0) {
                if (ch1 < 0xA0 || ch1 > 0xBF) return false;
            } else if (ch0 == 0xED) {
                if (ch1 < 0x80 || ch1 > 0x9F) return false;
            } else if (ch1 < 0x80 || ch1 > 0xBF) {
                return false;
            }
            *codePoint = ((ch0 & 0x0F) << 12) | ((ch1 & 0x3F) << 6) | (ch2 & 0x3F);
            *length = 3;
            return true;
        }

        if (ch0 >= 0xF0 && ch0 <= 0xF4) {
            if (offset + 3 >= size) return false;
            unsigned char ch1 = data[offset + 1];
            unsigned char ch2 = data[offset + 2];
            unsigned char ch3 = data[offset + 3];
            if (ch2 < 0x80 || ch2 > 0xBF || ch3 < 0x80 || ch3 > 0xBF) return false;
            if (ch0 == 0xF0) {
                if (ch1 < 0x90 || ch1 > 0xBF) return false;
            } else if (ch0 == 0xF4) {
                if (ch1 < 0x80 || ch1 > 0x8F) return false;
            } else if (ch1 < 0x80 || ch1 > 0xBF) {
                return false;
            }
            *codePoint = ((ch0 & 0x07) << 18) | ((ch1 & 0x3F) << 12) | ((ch2 & 0x3F) << 6) | (ch3 & 0x3F);
            *length = 4;
            return true;
        }

        return false;
    }

    bool isStrictUtf8(const unsigned char* data, size_t size, bool* hasMultibyte)
    {
        bool foundMultibyte = false;
        size_t offset = 0;

        while (offset < size) {
            uint32_t codePoint;
            size_t length;
            if (!decodeUtf8Character(data, size, offset, &codePoint, &length)) return false;
            if (length > 1) foundMultibyte = true;
            offset += length;
        }

        if (hasMultibyte != nullptr) *hasMultibyte = foundMultibyte;
        return true;
    }

    bool convertUtf8ToGbk(const unsigned char* data, size_t size, std::string* output)
    {
        const UnicodeToGbkMap& map = unicodeToGbkMap();
        if (!map.loaded) return false;

        output->clear();
        output->reserve(size);

        size_t offset = 0;
        while (offset < size) {
            uint32_t codePoint;
            size_t length;
            if (!decodeUtf8Character(data, size, offset, &codePoint, &length)) return false;
            offset += length;

            unsigned short encoded = codePoint < map.values.size() ? map.values[codePoint] : 0;
            if (encoded == 0 && codePoint != 0) encoded = kMissingGbkGlyph;

            if (encoded <= 0xFF) {
                output->push_back(static_cast<char>(encoded));
            } else {
                output->push_back(static_cast<char>((encoded >> 8) & 0xFF));
                output->push_back(static_cast<char>(encoded & 0xFF));
            }
        }

        return true;
    }

    bool isChsLanguage()
    {
        return compat_stricmp(settings.system.language.c_str(), "chs") == 0;
    }

} // namespace

bool textEncodingNormalize(const char* data, size_t size, std::string* output)
{
    if (output == nullptr || (data == nullptr && size != 0)) return false;

    output->assign(data != nullptr ? data : "", size);
    if (!isChsLanguage()) return true;

    const std::string& mode = settings.system.text_encoding;
    if (compat_stricmp(mode.c_str(), "legacy") == 0
        || compat_stricmp(mode.c_str(), "raw") == 0
        || compat_stricmp(mode.c_str(), "ansi") == 0
        || compat_stricmp(mode.c_str(), "gbk") == 0
        || compat_stricmp(mode.c_str(), "cp936") == 0) {
        return true;
    }

    static const unsigned char kEmpty = 0;
    const unsigned char* bytes = data != nullptr
        ? reinterpret_cast<const unsigned char*>(data)
        : &kEmpty;
    size_t offset = 0;
    bool hasBom = size >= sizeof(kUtf8Bom) && memcmp(bytes, kUtf8Bom, sizeof(kUtf8Bom)) == 0;
    if (hasBom) offset = sizeof(kUtf8Bom);

    bool hasMultibyte = false;
    bool validUtf8 = isStrictUtf8(bytes + offset, size - offset, &hasMultibyte);
    bool forceUtf8 = compat_stricmp(mode.c_str(), "utf8") == 0
        || compat_stricmp(mode.c_str(), "utf-8") == 0;
    bool autoMode = compat_stricmp(mode.c_str(), "auto") == 0 || mode.empty();

    if (!forceUtf8 && !autoMode) {
        debugPrint("Unknown system.text_encoding '%s'; using auto.\n", mode.c_str());
        autoMode = true;
    }

    bool useUtf8 = forceUtf8 || (autoMode && (hasBom || hasMultibyte));
    if (!useUtf8) return true;

    if (!validUtf8) {
        debugPrint("Localized text selected as UTF-8 contains an invalid byte sequence; preserving legacy bytes.\n");
        return true;
    }

    std::string converted;
    if (!convertUtf8ToGbk(bytes + offset, size - offset, &converted)) {
        debugPrint("Unable to convert localized UTF-8 text to GBK; preserving source bytes.\n");
        return true;
    }

    *output = std::move(converted);
    return true;
}

bool textEncodingLoadFile(const char* path, std::string* output)
{
    if (path == nullptr || output == nullptr) return false;

    File* stream = fileOpen(path, "rb");
    if (stream == nullptr) return false;

    int fileSize = fileGetSize(stream);
    if (fileSize < 0) {
        fileClose(stream);
        return false;
    }

    std::string raw;
    bool success = true;
    if (fileSize > 0) {
        raw.resize(static_cast<size_t>(fileSize));
        success = fileRead(raw.data(), 1, raw.size(), stream) == raw.size();
    } else {
        // Gzip-backed XFile streams do not report an uncompressed size.
        // Reading to EOF also handles ordinary empty files.
        char buffer[4096];
        size_t bytesRead;
        while ((bytesRead = fileRead(buffer, 1, sizeof(buffer), stream)) != 0) {
            raw.append(buffer, bytesRead);
        }
    }
    fileClose(stream);

    if (!success) return false;
    return textEncodingNormalize(raw.data(), raw.size(), output);
}

int textEncodingCharacterCount(const char* text)
{
    if (text == nullptr) return 0;

    int count = 0;
    const char* p = text;
    size_t remaining = strlen(text);
    while (remaining != 0) {
        size_t length = textEncodingCharacterLength(p, remaining);
        p += length;
        remaining -= length;
        count++;
    }

    return count;
}

size_t textEncodingCharacterLength(const char* text, size_t remaining)
{
    if (text == nullptr || remaining == 0) return 0;

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
    if (isChsLanguage()
        && remaining >= 2
        && bytes[0] >= 0x81
        && bytes[0] <= 0xFE
        && bytes[1] >= 0x40
        && bytes[1] <= 0xFE
        && bytes[1] != 0x7F) {
        return 2;
    }

    return 1;
}

} // namespace fallout
