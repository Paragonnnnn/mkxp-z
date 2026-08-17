#ifndef COSMOS_GDI_TEXT_H
#define COSMOS_GDI_TEXT_H

#include <SDL.h>
#include <SDL_ttf.h>

class Font;

namespace CosmosGDIText
{
    bool available();
    void registerFont(SDL_RWops &ops);

    SDL_Surface *renderText(TTF_Font *sdlFont,
                            Font &font,
                            const char *utf8,
                            const SDL_Color &color,
                            bool solid);

    SDL_Surface *renderOutline(TTF_Font *sdlFont,
                               Font &font,
                               const char *utf8,
                               const SDL_Color &color,
                               bool solid,
                               int outlineSize);

    bool measureText(TTF_Font *sdlFont,
                     Font &font,
                     const char *utf8,
                     int &width,
                     int &height);
}

#endif
