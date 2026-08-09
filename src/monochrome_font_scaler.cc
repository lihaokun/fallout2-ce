#include "monochrome_font_scaler.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fallout {

namespace {

    constexpr std::size_t kMaximumCacheEntries = 8192;
    constexpr std::size_t kMaximumCachePayloadSize = 2 * 1024 * 1024;

    typedef struct MonochromeGlyphCacheKey {
        const unsigned char* source;
        int sourceWidth;
        int sourceHeight;
        int sourcePitch;
        int targetWidth;
        int targetHeight;

        bool operator==(const MonochromeGlyphCacheKey& other) const
        {
            return source == other.source
                && sourceWidth == other.sourceWidth
                && sourceHeight == other.sourceHeight
                && sourcePitch == other.sourcePitch
                && targetWidth == other.targetWidth
                && targetHeight == other.targetHeight;
        }
    } MonochromeGlyphCacheKey;

    class MonochromeGlyphCacheKeyHash {
    public:
        std::size_t operator()(const MonochromeGlyphCacheKey& key) const
        {
            std::size_t hash = std::hash<const unsigned char*>()(key.source);
            hash = combine(hash, static_cast<std::size_t>(key.sourceWidth));
            hash = combine(hash, static_cast<std::size_t>(key.sourceHeight));
            hash = combine(hash, static_cast<std::size_t>(key.sourcePitch));
            hash = combine(hash, static_cast<std::size_t>(key.targetWidth));
            hash = combine(hash, static_cast<std::size_t>(key.targetHeight));
            return hash;
        }

    private:
        static std::size_t combine(std::size_t seed, std::size_t value)
        {
            return seed ^ (value + static_cast<std::size_t>(0x9E3779B9) + (seed << 6) + (seed >> 2));
        }
    };

    typedef std::unordered_map<MonochromeGlyphCacheKey, std::vector<unsigned char>, MonochromeGlyphCacheKeyHash> MonochromeGlyphCache;

    MonochromeGlyphCache gMonochromeGlyphCache;
    std::size_t gMonochromeGlyphCachePayloadSize = 0;

    unsigned char calculateCoverage(const MonochromeGlyphCacheKey& key, int targetX, int targetY)
    {
        // Coordinates are expressed in target-size units. In this coordinate
        // system every destination cell has an area of sourceWidth*sourceHeight,
        // which keeps the box filter exact without floating-point arithmetic.
        std::int64_t destinationLeft = static_cast<std::int64_t>(targetX) * key.sourceWidth;
        std::int64_t destinationRight = static_cast<std::int64_t>(targetX + 1) * key.sourceWidth;
        std::int64_t destinationTop = static_cast<std::int64_t>(targetY) * key.sourceHeight;
        std::int64_t destinationBottom = static_cast<std::int64_t>(targetY + 1) * key.sourceHeight;

        int sourceLeft = static_cast<int>(destinationLeft / key.targetWidth);
        int sourceRight = static_cast<int>((destinationRight + key.targetWidth - 1) / key.targetWidth);
        int sourceTop = static_cast<int>(destinationTop / key.targetHeight);
        int sourceBottom = static_cast<int>((destinationBottom + key.targetHeight - 1) / key.targetHeight);

        sourceLeft = std::clamp(sourceLeft, 0, key.sourceWidth);
        sourceRight = std::clamp(sourceRight, 0, key.sourceWidth);
        sourceTop = std::clamp(sourceTop, 0, key.sourceHeight);
        sourceBottom = std::clamp(sourceBottom, 0, key.sourceHeight);

        std::int64_t coveredArea = 0;
        for (int sourceY = sourceTop; sourceY < sourceBottom; sourceY++) {
            std::int64_t sourcePixelTop = static_cast<std::int64_t>(sourceY) * key.targetHeight;
            std::int64_t sourcePixelBottom = static_cast<std::int64_t>(sourceY + 1) * key.targetHeight;
            std::int64_t overlapY = std::min(destinationBottom, sourcePixelBottom) - std::max(destinationTop, sourcePixelTop);
            if (overlapY <= 0) {
                continue;
            }

            const unsigned char* sourceRow = key.source + static_cast<std::size_t>(sourceY) * key.sourcePitch;
            for (int sourceX = sourceLeft; sourceX < sourceRight; sourceX++) {
                if ((sourceRow[sourceX >> 3] & (0x80 >> (sourceX & 7))) == 0) {
                    continue;
                }

                std::int64_t sourcePixelLeft = static_cast<std::int64_t>(sourceX) * key.targetWidth;
                std::int64_t sourcePixelRight = static_cast<std::int64_t>(sourceX + 1) * key.targetWidth;
                std::int64_t overlapX = std::min(destinationRight, sourcePixelRight) - std::max(destinationLeft, sourcePixelLeft);
                if (overlapX > 0) {
                    coveredArea += overlapX * overlapY;
                }
            }
        }

        if (coveredArea == 0) {
            return 0;
        }

        std::int64_t destinationArea = static_cast<std::int64_t>(key.sourceWidth) * key.sourceHeight;
        std::int64_t coverage = (coveredArea * 7 + destinationArea / 2) / destinationArea;
        return static_cast<unsigned char>(std::clamp<std::int64_t>(coverage, 1, 7));
    }

} // namespace

bool monochromeFontGetDownscaledGlyphCoverage(const unsigned char* source,
    int sourceWidth,
    int sourceHeight,
    int sourcePitch,
    int targetWidth,
    int targetHeight,
    MonochromeGlyphCoverageView* view)
{
    if (view != nullptr) {
        view->width = 0;
        view->height = 0;
        view->coverage = nullptr;
    }

    if (source == nullptr
        || sourceWidth <= 0
        || sourceWidth > 4096
        || sourceHeight <= 0
        || sourceHeight > 4096
        || sourcePitch < (sourceWidth - 1) / 8 + 1
        || sourcePitch > 4096
        || targetWidth <= 0
        || targetWidth > 4096
        || targetHeight <= 0
        || targetHeight > 4096
        || (targetWidth >= sourceWidth && targetHeight >= sourceHeight)
        || view == nullptr) {
        return false;
    }

    std::size_t targetWidthSize = static_cast<std::size_t>(targetWidth);
    std::size_t targetHeightSize = static_cast<std::size_t>(targetHeight);
    if (targetWidthSize > std::numeric_limits<std::size_t>::max() / targetHeightSize) {
        return false;
    }

    std::size_t payloadSize = targetWidthSize * targetHeightSize;
    if (payloadSize == 0 || payloadSize > kMaximumCachePayloadSize) {
        return false;
    }

    MonochromeGlyphCacheKey key = {
        source,
        sourceWidth,
        sourceHeight,
        sourcePitch,
        targetWidth,
        targetHeight,
    };

    auto iterator = gMonochromeGlyphCache.find(key);
    if (iterator == gMonochromeGlyphCache.end()) {
        if (gMonochromeGlyphCache.size() >= kMaximumCacheEntries
            || gMonochromeGlyphCachePayloadSize + payloadSize > kMaximumCachePayloadSize) {
            monochromeFontScalerClearCache();
        }

        std::vector<unsigned char> coverage(payloadSize);
        for (int targetY = 0; targetY < targetHeight; targetY++) {
            for (int targetX = 0; targetX < targetWidth; targetX++) {
                coverage[static_cast<std::size_t>(targetY) * targetWidthSize + targetX] = calculateCoverage(key, targetX, targetY);
            }
        }

        auto result = gMonochromeGlyphCache.emplace(key, std::move(coverage));
        iterator = result.first;
        gMonochromeGlyphCachePayloadSize += payloadSize;
    }

    view->width = targetWidth;
    view->height = targetHeight;
    view->coverage = iterator->second.data();
    return true;
}

void monochromeFontScalerClearCache()
{
    gMonochromeGlyphCache.clear();
    gMonochromeGlyphCachePayloadSize = 0;
}

} // namespace fallout
