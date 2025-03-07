#include "SdlRenderer.h"

#include "SDL_image.h"
#include "Silkscreen.h"

namespace {
    SDL_Texture* loadSilkscreen(SDL_Renderer* renderer) {
        SDL_RWops* rwops = SDL_RWFromConstMem((const void*)silkscreenPng_data, silkscreenPng_len);
        SDL_Surface* surface = IMG_LoadPNG_RW(rwops);
        SDL_RWclose(rwops);

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);

        SDL_FreeSurface(surface);

        return texture;
    }
}  // namespace

SdlRenderer::SdlRenderer(SDL_Window* window, SDL_Renderer* renderer, SoC* soc, int scale)
    : window(window), renderer(renderer), soc(soc), scale(scale) {
    deviceGetDisplayConfiguration(socGetDeviceType(soc), &displayConfiguration);

    frameTexture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                          displayConfiguration.width, displayConfiguration.height);

    if (displayConfiguration.graffitiHeight > 0) {
        silkscreenTexture = loadSilkscreen(renderer);
    }

    SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0xff);
    SDL_RenderClear(renderer);
    DrawSilkscreen();
    SDL_RenderPresent(renderer);
}

void SdlRenderer::Draw(bool forceRedraw) {
    uint32_t* frame = socGetPendingFrame(soc);
    if (!frame && !forceRedraw) return;

    if (frame) {
        uint8_t* pixels;
        int pitch;
        SDL_LockTexture(frameTexture, NULL, (void**)&pixels, &pitch);

        if (pitch == 4 * displayConfiguration.width) {
            memcpy(pixels, frame, 4 * displayConfiguration.width * displayConfiguration.height);
        } else {
            for (int y = 0; y < displayConfiguration.height; y++) {
                memcpy(pixels, frame, 4 * displayConfiguration.width);
                frame += displayConfiguration.width;
                pixels += pitch;
            }
        }

        SDL_UnlockTexture(frameTexture);
        frameTextureValid = true;
    }

    SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0xff);
    SDL_RenderClear(renderer);
    DrawSilkscreen();

    if (frameTextureValid) {
        SDL_Rect dest = {.x = 0,
                         .y = 0,
                         .w = scale * displayConfiguration.width,
                         .h = scale * displayConfiguration.height};
        SDL_RenderCopy(renderer, frameTexture, nullptr, &dest);
    }

    SDL_RenderPresent(renderer);

    socResetPendingFrame(soc);
}

void SdlRenderer::DrawSilkscreen() {
    if (!silkscreenTexture) return;

    SDL_Rect dest = {.x = 0,
                     .y = scale * displayConfiguration.height,
                     .w = scale * displayConfiguration.width,
                     .h = scale * displayConfiguration.graffitiHeight};

    SDL_RenderCopy(renderer, silkscreenTexture, nullptr, &dest);
}
