/* Keyboard (evdev through SDL KMSDRM) + HDMI audio probe: prints key events for ~20 s, plays a tone. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
static void cb(void *u, Uint8 *s, int len) {
    static double ph; Sint16 *o = (Sint16 *)s; int n = len / 4;
    for (int i = 0; i < n; i++) { Sint16 v = (Sint16)(sin(ph) * 8000); ph += 2 * 3.14159265 * 440 / 48000; o[2*i] = o[2*i+1] = v; }
    (void)u;
}
int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { printf("init: %s\n", SDL_GetError()); return 1; }
    printf("video=%s\n", SDL_GetCurrentVideoDriver());
    SDL_Window *w = SDL_CreateWindow("t", 0, 0, 640, 480, SDL_WINDOW_OPENGL);
    printf("window %p (%s)\n", (void *)w, w ? "ok" : SDL_GetError());
    SDL_AudioSpec want = {0}, got; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = cb;
    SDL_AudioDeviceID d = 0; (void)want; (void)got;
    
    if (d) SDL_PauseAudioDevice(d, 0);
    Uint32 t0 = SDL_GetTicks(); int n = 0;
    while (SDL_GetTicks() - t0 < 20000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) { printf("key %s %s (scancode %d)\n", e.type == SDL_KEYDOWN ? "down" : "up  ", SDL_GetKeyName(e.key.keysym.sym), e.key.keysym.scancode); n++; fflush(stdout); }
        }
        SDL_Delay(5);
        if (SDL_GetTicks() - t0 > 4000 && d) { SDL_CloseAudioDevice(d); d = 0; printf("tone done\n"); }
    }
    printf("%d key events\n", n); return 0;
}
