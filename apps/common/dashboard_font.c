/** @file
 * @brief Platform-matched native dashboard font loading. */
#include "dashboard_font.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** Read one big-endian 16-bit SFNT value. */
static uint16_t readBe16(const unsigned char *value) {
    return (uint16_t)(((uint16_t)value[0] << 8U) | value[1]);
}

/** Read one big-endian 32-bit SFNT value. */
static uint32_t readBe32(const unsigned char *value) {
    return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) | value[3];
}

/** Write one big-endian 32-bit SFNT value. */
static void writeBe32(unsigned char *output, uint32_t value) {
    output[0] = (unsigned char)(value >> 24U);
    output[1] = (unsigned char)(value >> 16U);
    output[2] = (unsigned char)(value >> 8U);
    output[3] = (unsigned char)value;
}

/** Extract one TTC face into a standalone in-memory SFNT font. */
static unsigned char *extractTtcFace(const unsigned char *collection,
                                     size_t collectionSize, uint32_t faceIndex,
                                     int *fontSize) {
    if (collectionSize < 12U || memcmp(collection, "ttcf", 4U) != 0) {
        return NULL;
    }
    const uint32_t faceCount = readBe32(collection + 8U);
    if (faceIndex >= faceCount || 12U + (size_t)faceCount * 4U > collectionSize) {
        return NULL;
    }
    const uint32_t faceOffset = readBe32(collection + 12U + faceIndex * 4U);
    if ((size_t)faceOffset + 12U > collectionSize) {
        return NULL;
    }
    const uint16_t tableCount = readBe16(collection + faceOffset + 4U);
    const size_t directorySize = 12U + (size_t)tableCount * 16U;
    if ((size_t)faceOffset + directorySize > collectionSize) {
        return NULL;
    }

    size_t outputSize = directorySize;
    for (uint16_t table = 0U; table < tableCount; ++table) {
        const unsigned char *record = collection + faceOffset + 12U + (size_t)table * 16U;
        const uint32_t offset = readBe32(record + 8U);
        const uint32_t length = readBe32(record + 12U);
        if ((size_t)offset + length > collectionSize) {
            return NULL;
        }
        outputSize += ((size_t)length + 3U) & ~(size_t)3U;
    }
    if (outputSize > INT32_MAX) {
        return NULL;
    }

    unsigned char *output = calloc(outputSize, 1U);
    if (output == NULL) {
        return NULL;
    }
    memcpy(output, collection + faceOffset, directorySize);
    size_t destinationOffset = directorySize;
    for (uint16_t table = 0U; table < tableCount; ++table) {
        const unsigned char *sourceRecord =
            collection + faceOffset + 12U + (size_t)table * 16U;
        unsigned char *destinationRecord = output + 12U + (size_t)table * 16U;
        const uint32_t sourceOffset = readBe32(sourceRecord + 8U);
        const uint32_t length = readBe32(sourceRecord + 12U);
        writeBe32(destinationRecord + 8U, (uint32_t)destinationOffset);
        memcpy(output + destinationOffset, collection + sourceOffset, length);
        destinationOffset += ((size_t)length + 3U) & ~(size_t)3U;
    }
    *fontSize = (int)outputSize;
    return output;
}

/** Load a regular font file or one indexed face from a TTC collection. */
static Font loadFace(const char *path, uint32_t faceIndex, bool collection,
                     int pixelSize, bool *owned) {
    Font font = {0};
    *owned = false;
    if (!FileExists(path)) {
        return font;
    }
    if (!collection) {
        font = LoadFontEx(path, pixelSize, NULL, 0);
    } else {
        int collectionSize = 0;
        unsigned char *fileData = LoadFileData(path, &collectionSize);
        int faceSize = 0;
        unsigned char *face = fileData == NULL ? NULL : extractTtcFace(
            fileData, (size_t)collectionSize, faceIndex, &faceSize);
        if (face != NULL) {
            font = LoadFontFromMemory(".ttf", face, faceSize,
                                      pixelSize, NULL, 0);
        }
        free(face);
        if (fileData != NULL) {
            UnloadFileData(fileData);
        }
    }
    *owned = font.texture.id != 0U &&
             font.texture.id != GetFontDefault().texture.id;
    if (*owned) {
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    }
    return font;
}

/** Load a fallback font if the platform-native selection failed. */
static void loadFallback(Font *font, bool *owned, const char *path, int pixelSize) {
    *font = loadFace(path, 0U, false, pixelSize, owned);
    if (!*owned) {
        *font = GetFontDefault();
        SetTextureFilter(font->texture, TEXTURE_FILTER_BILINEAR);
    }
}

/** Load one platform face at one exact output size. */
static Font loadPlatformFace(bool bold, int pixelSize, bool *owned,
                             const char *fallback) {
    Font font = {0};
#if defined(__APPLE__)
    const char *platformPath = "/System/Library/Fonts/Avenir Next.ttc";
    font = loadFace(platformPath, bold ? 0U : 7U, true, pixelSize, owned);
#elif defined(_WIN32)
    font = loadFace(bold ? "C:/Windows/Fonts/segoeuib.ttf"
                         : "C:/Windows/Fonts/segoeui.ttf",
                    0U, false, pixelSize, owned);
#else
    font = loadFace(bold
                        ? "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
                        : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    0U, false, pixelSize, owned);
#endif
    if (!*owned) {
        loadFallback(&font, owned, fallback, pixelSize);
    }
    return font;
}

int AppDashboardFont_RenderSize(int pythonPixels) {
#if defined(__APPLE__)
    /* Avenir Next: 1000 UPM, 1000 ascent, 366 descent. */
    return (pythonPixels * 1366 + 500) / 1000;
#elif defined(_WIN32)
    /* Segoe UI's typographic line box is approximately 1.22 em. */
    return (pythonPixels * 1220 + 500) / 1000;
#else
    /* DejaVu Sans: 2048 UPM and a 2384-unit typographic line box. */
    return (pythonPixels * 2384 + 1024) / 2048;
#endif
}

void AppDashboardFont_Load(AppDashboardFontSet_t *fonts,
                           const char *fallbackRegular, const char *fallbackBold) {
    memset(fonts, 0, sizeof(*fonts));
    const Vector2 dpi = GetWindowScaleDPI();
    const float scale = dpi.x > dpi.y ? dpi.x : dpi.y;
#define PHYSICAL_SIZE(logical) ((int)((float)AppDashboardFont_RenderSize(logical) * scale + 0.5f))
    fonts->regular12 = loadPlatformFace(false, PHYSICAL_SIZE(12),
                                        &fonts->owned[0], fallbackRegular);
    fonts->regular15 = loadPlatformFace(false, PHYSICAL_SIZE(15),
                                        &fonts->owned[1], fallbackRegular);
    fonts->regular18 = loadPlatformFace(false, PHYSICAL_SIZE(18),
                                        &fonts->owned[2], fallbackRegular);
    fonts->bold12 = loadPlatformFace(true, PHYSICAL_SIZE(12),
                                     &fonts->owned[3], fallbackBold);
    fonts->bold14 = loadPlatformFace(true, PHYSICAL_SIZE(14),
                                     &fonts->owned[4], fallbackBold);
    fonts->bold16 = loadPlatformFace(true, PHYSICAL_SIZE(16),
                                     &fonts->owned[5], fallbackBold);
    fonts->bold22 = loadPlatformFace(true, PHYSICAL_SIZE(22),
                                     &fonts->owned[6], fallbackBold);
    fonts->bold28 = loadPlatformFace(true, PHYSICAL_SIZE(28),
                                     &fonts->owned[7], fallbackBold);
#undef PHYSICAL_SIZE
}

void AppDashboardFont_Unload(AppDashboardFontSet_t *fonts) {
    Font *faces[] = {&fonts->regular12, &fonts->regular15, &fonts->regular18,
                     &fonts->bold12, &fonts->bold14, &fonts->bold16,
                     &fonts->bold22, &fonts->bold28};
    for (size_t index = 0U; index < sizeof(faces) / sizeof(faces[0]); ++index) {
        if (fonts->owned[index]) {
            UnloadFont(*faces[index]);
            fonts->owned[index] = false;
        }
    }
}

Font AppDashboardFont_Regular(const AppDashboardFontSet_t *fonts, int size) {
    if (size == 12) {
        return fonts->regular12;
    }
    return size == 18 ? fonts->regular18 : fonts->regular15;
}

Font AppDashboardFont_Bold(const AppDashboardFontSet_t *fonts, int size) {
    switch (size) {
        case 12: return fonts->bold12;
        case 14: return fonts->bold14;
        case 22: return fonts->bold22;
        case 28: return fonts->bold28;
        default: return fonts->bold16;
    }
}

void AppDashboardFont_DrawCentered(Font font, const char *text,
                                   Rectangle bounds, float fontSize,
                                   Color color) {
    const float scale = fontSize / (float)font.baseSize;
    float cursor = 0.0f;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool hasInk = false;
    for (const char *current = text; *current != '\0';) {
        int bytes = 0;
        const int codepoint = GetCodepointNext(current, &bytes);
        if (bytes <= 0) {
            bytes = 1;
        }
        const int index = GetGlyphIndex(font, codepoint);
        const GlyphInfo glyph = font.glyphs[index];
        const Rectangle glyphRectangle = font.recs[index];
        if (glyphRectangle.width > 0.0f && glyphRectangle.height > 0.0f) {
            const float glyphLeft = cursor + (float)glyph.offsetX * scale;
            const float glyphTop = (float)glyph.offsetY * scale;
            const float glyphRight = glyphLeft + glyphRectangle.width * scale;
            const float glyphBottom = glyphTop + glyphRectangle.height * scale;
            if (!hasInk) {
                left = glyphLeft;
                top = glyphTop;
                right = glyphRight;
                bottom = glyphBottom;
                hasInk = true;
            } else {
                if (glyphLeft < left) left = glyphLeft;
                if (glyphTop < top) top = glyphTop;
                if (glyphRight > right) right = glyphRight;
                if (glyphBottom > bottom) bottom = glyphBottom;
            }
        }
        cursor += (glyph.advanceX != 0
                       ? (float)glyph.advanceX
                       : glyphRectangle.width + (float)glyph.offsetX) * scale;
        current += bytes;
    }
    if (!hasInk) {
        return;
    }
    const Vector2 origin = {
        bounds.x + (bounds.width - (right - left)) * 0.5f - left,
        bounds.y + (bounds.height - (bottom - top)) * 0.5f - top,
    };
    DrawTextEx(font, text, origin, fontSize, 0.0f, color);
}
