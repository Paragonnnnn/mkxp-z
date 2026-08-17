/*
 * Cosmos RGSS301 text renderer for mkxp-z
 *
 * Windows-only optional path using classic GDI rasterization.
 * Other platforms, failures, and unsupported fonts fall back to SDL_ttf.
 */

#include "cosmos_gdi_text.h"
#include "font.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    struct PrivateFontStore
    {
        std::vector<HANDLE> handles;
        std::vector<std::vector<unsigned char>> buffers;

        ~PrivateFontStore()
        {
            for (HANDLE handle : handles)
            {
                if (handle)
                    RemoveFontMemResourceEx(handle);
            }
        }
    };

    PrivateFontStore &fontStore()
    {
        static PrivateFontStore store;
        return store;
    }

    std::wstring utf8ToWide(const char *text)
    {
        if (!text || !*text)
            return std::wstring();

        int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (count <= 1)
            return std::wstring();

        std::wstring out((size_t)count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], count);
        if (!out.empty() && out.back() == L'\0')
            out.pop_back();
        return out;
    }

    std::wstring familyFromTTF(TTF_Font *sdlFont)
    {
        if (!sdlFont)
            return std::wstring();

        const char *family = TTF_FontFaceFamilyName(sdlFont);
        if (!family || !*family)
            return std::wstring();

        return utf8ToWide(family);
    }

    struct GDIContext
    {
        HDC dc;
        HFONT font;
        HGDIOBJ oldFont;
        std::wstring text;
        SIZE extent;
        TEXTMETRICW metrics;

        GDIContext()
            : dc(nullptr), font(nullptr), oldFont(nullptr), extent{0,0}, metrics{}
        {}

        ~GDIContext()
        {
            if (dc && oldFont)
                SelectObject(dc, oldFont);
            if (font)
                DeleteObject(font);
            if (dc)
                DeleteDC(dc);
        }
    };

    bool prepareContext(TTF_Font *sdlFont, Font &font, const char *utf8, GDIContext &ctx)
    {
        ctx.text = utf8ToWide(utf8);
        if (ctx.text.empty())
            return false;

        const std::wstring family = familyFromTTF(sdlFont);
        if (family.empty())
            return false;

        ctx.dc = CreateCompatibleDC(nullptr);
        if (!ctx.dc)
            return false;

        /*
         * RGSS3 uses Windows font semantics. mkxp-z already reverse-engineers
         * Windows's positive-height selection in font.cpp; using Font#size as
         * a positive GDI cell height keeps the native Windows path consistent.
         */
        int requestedHeight = std::max(font.getSize(), 1);

        ctx.font = CreateFontW(
            requestedHeight,                         // height (positive = cell height)
            0,                                       // width
            0,                                       // escapement
            0,                                       // orientation
            font.getBold() ? FW_BOLD : FW_NORMAL,
            font.getItalic() ? TRUE : FALSE,
            FALSE,                                   // underline
            FALSE,                                   // strikeout
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            family.c_str()
        );

        if (!ctx.font)
            return false;

        ctx.oldFont = SelectObject(ctx.dc, ctx.font);
        if (!ctx.oldFont)
            return false;

        SetTextAlign(ctx.dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        SetBkMode(ctx.dc, TRANSPARENT);

        if (!GetTextMetricsW(ctx.dc, &ctx.metrics))
            return false;

        if (!GetTextExtentPoint32W(ctx.dc, ctx.text.c_str(),
                                   (int)ctx.text.size(), &ctx.extent))
            return false;

        /*
         * GDI silently substitutes a different face when a requested family
         * is unavailable. In that case we deliberately fall back to SDL_ttf.
         */
        wchar_t actualFace[LF_FACESIZE] = {0};
        if (GetTextFaceW(ctx.dc, LF_FACESIZE, actualFace) > 0)
        {
            if (_wcsicmp(actualFace, family.c_str()) != 0)
                return false;
        }

        return true;
    }

    SDL_Surface *surfaceFromGDI(GDIContext &ctx,
                                const SDL_Color &color,
                                bool solid)
    {
        int width  = std::max<int>(ctx.extent.cx, 1);
        int height = std::max<int>(ctx.metrics.tmHeight, 1);

        BITMAPINFO bmi;
        std::memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void *dibPixels = nullptr;
        HBITMAP dib = CreateDIBSection(ctx.dc, &bmi, DIB_RGB_COLORS,
                                       &dibPixels, nullptr, 0);
        if (!dib || !dibPixels)
        {
            if (dib)
                DeleteObject(dib);
            return nullptr;
        }

        HGDIOBJ oldBitmap = SelectObject(ctx.dc, dib);
        if (!oldBitmap)
        {
            DeleteObject(dib);
            return nullptr;
        }

        std::memset(dibPixels, 0, (size_t)width * (size_t)height * 4);
        SetTextColor(ctx.dc, RGB(255, 255, 255));
        SetBkColor(ctx.dc, RGB(0, 0, 0));
        SetBkMode(ctx.dc, TRANSPARENT);

        BOOL ok = TextOutW(ctx.dc, 0, 0, ctx.text.c_str(), (int)ctx.text.size());

        SelectObject(ctx.dc, oldBitmap);

        if (!ok)
        {
            DeleteObject(dib);
            return nullptr;
        }

        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
            0, width, height, 32, SDL_PIXELFORMAT_ABGR8888);

        if (!surface)
        {
            DeleteObject(dib);
            return nullptr;
        }

        const uint8_t *src = static_cast<const uint8_t *>(dibPixels);

        for (int y = 0; y < height; ++y)
        {
            uint32_t *dst = reinterpret_cast<uint32_t *>(
                static_cast<uint8_t *>(surface->pixels) + y * surface->pitch);

            for (int x = 0; x < width; ++x)
            {
                const uint8_t *px = src + ((size_t)y * width + x) * 4;

                // DIB is B,G,R,0. With white-on-black grayscale AA,
                // the largest component is the coverage.
                uint8_t coverage = std::max(px[0], std::max(px[1], px[2]));

                if (solid)
                    coverage = coverage >= 128 ? 255 : 0;

                uint8_t alpha = (uint8_t)(((unsigned int)coverage * color.a) / 255);

                dst[x] = SDL_MapRGBA(surface->format,
                                     color.r, color.g, color.b, alpha);
            }
        }

        DeleteObject(dib);
        return surface;
    }

    SDL_Surface *makeDiagonalOutline(SDL_Surface *base,
                                     const SDL_Color &color,
                                     int radius)
    {
        if (!base || radius <= 0)
            return nullptr;

        const int outW = base->w + radius * 2;
        const int outH = base->h + radius * 2;

        SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(
            0, outW, outH, 32, SDL_PIXELFORMAT_ABGR8888);

        if (!out)
            return nullptr;

        SDL_FillRect(out, nullptr,
                     SDL_MapRGBA(out->format, color.r, color.g, color.b, 0));

        const int offsets[4][2] = {
            {0, 0},
            {radius * 2, 0},
            {0, radius * 2},
            {radius * 2, radius * 2}
        };

        for (int y = 0; y < base->h; ++y)
        {
            const uint32_t *src = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(base->pixels) + y * base->pitch);

            for (int x = 0; x < base->w; ++x)
            {
                uint8_t r, g, b, a;
                SDL_GetRGBA(src[x], base->format, &r, &g, &b, &a);
                if (!a)
                    continue;

                for (int i = 0; i < 4; ++i)
                {
                    int dx = x + offsets[i][0];
                    int dy = y + offsets[i][1];

                    uint32_t *dst = reinterpret_cast<uint32_t *>(
                        static_cast<uint8_t *>(out->pixels) + dy * out->pitch) + dx;

                    uint8_t dr, dg, db, da;
                    SDL_GetRGBA(*dst, out->format, &dr, &dg, &db, &da);

                    // Opaque outlines simply need max coverage. For translucent
                    // outlines this still stays close to RGSS and the existing
                    // mkxp-z blendText stage handles the final composition.
                    uint8_t na = std::max(da, a);
                    *dst = SDL_MapRGBA(out->format,
                                      color.r, color.g, color.b, na);
                }
            }
        }

        return out;
    }
}

namespace CosmosGDIText
{
    bool available()
    {
        return true;
    }

    void registerFont(SDL_RWops &ops)
    {
        Sint64 oldPos = SDL_RWtell(&ops);
        Sint64 size = SDL_RWsize(&ops);

        if (size <= 0 || size > 0x7fffffff)
            return;

        std::vector<unsigned char> data((size_t)size);

        if (SDL_RWseek(&ops, 0, RW_SEEK_SET) < 0)
            return;

        size_t read = SDL_RWread(&ops, data.data(), 1, (size_t)size);

        if (oldPos >= 0)
            SDL_RWseek(&ops, oldPos, RW_SEEK_SET);

        if (read != (size_t)size)
            return;

        DWORD fontCount = 0;
        HANDLE handle = AddFontMemResourceEx(data.data(), (DWORD)data.size(),
                                             nullptr, &fontCount);

        if (!handle || fontCount == 0)
            return;

        fontStore().handles.push_back(handle);
        fontStore().buffers.push_back(std::move(data));
    }

    bool measureText(TTF_Font *sdlFont,
                     Font &font,
                     const char *utf8,
                     int &width,
                     int &height)
    {
        GDIContext ctx;
        if (!prepareContext(sdlFont, font, utf8, ctx))
            return false;

        width = std::max<int>(ctx.extent.cx, 0);
        height = width ? std::max<int>(ctx.metrics.tmHeight, 0) : 0;
        return true;
    }

    SDL_Surface *renderText(TTF_Font *sdlFont,
                            Font &font,
                            const char *utf8,
                            const SDL_Color &color,
                            bool solid)
    {
        GDIContext ctx;
        if (!prepareContext(sdlFont, font, utf8, ctx))
            return nullptr;

        return surfaceFromGDI(ctx, color, solid);
    }

    SDL_Surface *renderOutline(TTF_Font *sdlFont,
                               Font &font,
                               const char *utf8,
                               const SDL_Color &color,
                               bool solid,
                               int outlineSize)
    {
        SDL_Surface *base = renderText(sdlFont, font, utf8, color, solid);
        if (!base)
            return nullptr;

        SDL_Surface *outline = makeDiagonalOutline(base, color, outlineSize);
        SDL_FreeSurface(base);
        return outline;
    }
}

#else

namespace CosmosGDIText
{
    bool available() { return false; }
    void registerFont(SDL_RWops &) {}

    SDL_Surface *renderText(TTF_Font *, Font &, const char *,
                            const SDL_Color &, bool)
    {
        return nullptr;
    }

    SDL_Surface *renderOutline(TTF_Font *, Font &, const char *,
                               const SDL_Color &, bool, int)
    {
        return nullptr;
    }

    bool measureText(TTF_Font *, Font &, const char *, int &, int &)
    {
        return false;
    }
}

#endif
