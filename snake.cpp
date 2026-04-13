#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

namespace {

const int SCREEN_WIDTH = 600;
const int SCREEN_HEIGHT = 400;
const int FPS = 60;
const int FRAME_DELAY = 1000 / FPS;

const int CELL = 15;
const int HUD_H = 36;
const int GRID_W = SCREEN_WIDTH / CELL;
const int GRID_H = (SCREEN_HEIGHT - HUD_H) / CELL;

const Uint32 MOVE_MS_START = 130;
const Uint32 MOVE_MS_MIN = 55;

enum class Mode { Menu, Playing, GameOver };

struct Point {
    int x;
    int y;
};

SDL_Color nokiaGreen = {155, 188, 15, 255};
SDL_Color nokiaDark = {15, 56, 15, 255};
SDL_Color foodColor = {220, 60, 40, 255};

// ---------------------------------------------------------------------------
// Famicom-style dynamic bleeps (SDL audio callback, square + LFSR noise)
// ---------------------------------------------------------------------------
namespace Beeper {

constexpr int SR = 44100;
constexpr int MAXV = 8;

struct Voice {
    bool on = false;
    int pos = 0;
    int len = 0;
    float phase = 0.f;
    float f0 = 440.f;
    float f1 = 440.f;
    float amp = 0.25f;
    float duty = 0.25f;  // square duty ~NES pulse "25%"
    bool noise = false;
    Uint32 lfsr = 0xACE1u;
};

static Voice g_voices[MAXV];
static SDL_AudioDeviceID g_dev = 0;

static int ms_to_samples(int ms) { return (ms * SR) / 1000; }

static int alloc_voice() {
    for (int i = 0; i < MAXV; ++i) {
        if (!g_voices[i].on) {
            return i;
        }
    }
    return MAXV - 1;
}

static void mix(void*, Uint8* stream, int len) {
    auto* out = reinterpret_cast<Sint16*>(stream);
    const int frames = len / (static_cast<int>(sizeof(Sint16)) * 2);

    for (int i = 0; i < frames; ++i) {
        float acc = 0.f;

        for (int v = 0; v < MAXV; ++v) {
            Voice& vo = g_voices[v];
            if (!vo.on) {
                continue;
            }
            if (vo.pos >= vo.len) {
                vo.on = false;
                continue;
            }

            const float u = static_cast<float>(vo.pos) / static_cast<float>(std::max(1, vo.len - 1));
            const float hz = vo.f0 + (vo.f1 - vo.f0) * u;
            const float env = 1.f - u;

            float s = 0.f;
            if (vo.noise) {
                vo.lfsr = vo.lfsr * 1664525u + 1013904223u;
                s = ((vo.lfsr >> 9) & 1u) ? 0.5f : -0.5f;
            } else {
                vo.phase += hz / static_cast<float>(SR);
                if (vo.phase >= 1.f) {
                    vo.phase -= std::floor(vo.phase);
                }
                s = (vo.phase < vo.duty) ? 0.5f : -0.5f;
            }

            acc += s * vo.amp * env;
            ++vo.pos;
        }

        acc = std::tanh(acc * 1.4f);
        const Sint16 sample =
            static_cast<Sint16>(std::clamp(acc * 26000.f, -30000.f, 30000.f));
        out[i * 2] = sample;
        out[i * 2 + 1] = sample;
    }
}

static bool init() {
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = SR;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = mix;
    want.userdata = nullptr;

    SDL_AudioSpec have;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (g_dev == 0) {
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

static void shutdown() {
    if (g_dev != 0) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
}

static void voice_square(int dur_ms, float hz0, float hz1, float amp, float duty) {
    if (g_dev == 0) {
        return;
    }
    SDL_LockAudioDevice(g_dev);
    const int i = alloc_voice();
    Voice& vo = g_voices[i];
    vo = Voice{};
    vo.on = true;
    vo.len = ms_to_samples(dur_ms);
    vo.f0 = hz0;
    vo.f1 = hz1;
    vo.amp = amp;
    vo.duty = duty;
    vo.noise = false;
    SDL_UnlockAudioDevice(g_dev);
}

static void voice_noise(int dur_ms, float amp) {
    if (g_dev == 0) {
        return;
    }
    SDL_LockAudioDevice(g_dev);
    const int i = alloc_voice();
    Voice& vo = g_voices[i];
    vo = Voice{};
    vo.on = true;
    vo.len = ms_to_samples(dur_ms);
    vo.f0 = vo.f1 = 1000.f;
    vo.amp = amp;
    vo.noise = true;
    vo.lfsr = static_cast<Uint32>(SDL_GetTicks()) | 1u;
    SDL_UnlockAudioDevice(g_dev);
}

// --- Presets (dynamic: eat pitch rises with score) ---
static void sfx_menu() {
    voice_square(55, 196.f, 392.f, 0.14f, 0.125f);
    voice_square(55, 262.f, 523.f, 0.12f, 0.25f);
    voice_square(70, 330.f, 784.f, 0.1f, 0.5f);
}

static void sfx_move() {
    voice_square(18, 180.f, 95.f, 0.06f, 0.125f);
}

static void sfx_turn() {
    voice_square(12, 420.f, 260.f, 0.05f, 0.25f);
}

static void sfx_eat(int score) {
    const float bump = static_cast<float>(std::min(score, 40)) * 6.f;
    voice_square(55, 320.f + bump, 980.f + bump * 1.5f, 0.18f, 0.25f);
    voice_square(40, 660.f + bump * 0.5f, 1320.f + bump, 0.1f, 0.125f);
}

static void sfx_die() {
    voice_noise(120, 0.2f);
    voice_square(220, 420.f, 55.f, 0.2f, 0.25f);
    voice_square(160, 315.f, 40.f, 0.14f, 0.5f);
}

static void sfx_pause_menu() {
    voice_square(60, 200.f, 120.f, 0.08f, 0.25f);
}

}  // namespace Beeper

TTF_Font* load_font(int pt) {
    const char* paths[] = {
        "arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/Library/Fonts/Arial.ttf",
        nullptr,
    };
    for (int i = 0; paths[i]; ++i) {
        TTF_Font* f = TTF_OpenFont(paths[i], pt);
        if (f) {
            return f;
        }
    }
    return nullptr;
}

void draw_text(SDL_Renderer* r, TTF_Font* font, const std::string& text, int x, int y,
               SDL_Color fg, SDL_Color* bg) {
    if (!font || text.empty()) {
        return;
    }
    SDL_Surface* surf =
        bg ? TTF_RenderText_Shaded(font, text.c_str(), fg, *bg)
           : TTF_RenderText_Solid(font, text.c_str(), fg);
    if (!surf) {
        return;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return;
    }
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void fill_cell(SDL_Renderer* r, int gx, int gy, SDL_Color c) {
    SDL_Rect rect = {gx * CELL, HUD_H + gy * CELL, CELL - 1, CELL - 1};
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
}

bool cell_occupied(const std::deque<Point>& snake, int x, int y) {
    for (const Point& p : snake) {
        if (p.x == x && p.y == y) {
            return true;
        }
    }
    return false;
}

void spawn_food(Point& food, const std::deque<Point>& snake) {
    for (int tries = 0; tries < 5000; ++tries) {
        food.x = rand() % GRID_W;
        food.y = rand() % GRID_H;
        if (!cell_occupied(snake, food.x, food.y)) {
            return;
        }
    }
}

void reset_game(std::deque<Point>& snake, Point& dir, Point& pending, Point& food, int& score,
                Uint32& move_ms) {
    snake.clear();
    int cx = GRID_W / 2;
    int cy = GRID_H / 2;
    snake.push_back({cx, cy});
    snake.push_back({cx - 1, cy});
    snake.push_back({cx - 2, cy});
    dir = {1, 0};
    pending = dir;
    score = 0;
    move_ms = MOVE_MS_START;
    spawn_food(food, snake);
}

}  // namespace

int main(int argc, char* args[]) {
    (void)argc;
    (void)args;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0) {
        return 1;
    }
    if (TTF_Init() < 0) {
        SDL_Quit();
        return 1;
    }

    Beeper::init();

    SDL_Window* window =
        SDL_CreateWindow("AC Holding Snake", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        Beeper::shutdown();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        Beeper::shutdown();
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    TTF_Font* font = load_font(22);
    TTF_Font* fontBig = load_font(28);

    Mode mode = Mode::Menu;
    std::deque<Point> snake;
    Point dir{1, 0};
    Point pending{1, 0};
    Point pending_prev = pending;
    Point food{0, 0};
    int score = 0;
    Uint32 move_ms = MOVE_MS_START;
    Uint32 last_move = 0;

    srand(static_cast<unsigned>(time(nullptr)));

    bool quit = false;
    SDL_Event e;

    while (!quit) {
        Uint32 frameStart = SDL_GetTicks();

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
            }
            if (e.type != SDL_KEYDOWN) {
                continue;
            }
            const SDL_Keycode k = e.key.keysym.sym;

            if (mode == Mode::Menu) {
                if (k == SDLK_RETURN || k == SDLK_SPACE) {
                    reset_game(snake, dir, pending, food, score, move_ms);
                    last_move = SDL_GetTicks();
                    mode = Mode::Playing;
                    Beeper::sfx_menu();
                }
                continue;
            }

            if (mode == Mode::GameOver) {
                if (k == SDLK_RETURN || k == SDLK_SPACE) {
                    reset_game(snake, dir, pending, food, score, move_ms);
                    last_move = SDL_GetTicks();
                    mode = Mode::Playing;
                    Beeper::sfx_menu();
                } else if (k == SDLK_ESCAPE) {
                    mode = Mode::Menu;
                    Beeper::sfx_pause_menu();
                }
                continue;
            }

            pending_prev = pending;
            if (k == SDLK_UP || k == SDLK_w) {
                if (dir.y == 0) {
                    pending = {0, -1};
                }
            } else if (k == SDLK_DOWN || k == SDLK_s) {
                if (dir.y == 0) {
                    pending = {0, 1};
                }
            } else if (k == SDLK_LEFT || k == SDLK_a) {
                if (dir.x == 0) {
                    pending = {-1, 0};
                }
            } else if (k == SDLK_RIGHT || k == SDLK_d) {
                if (dir.x == 0) {
                    pending = {1, 0};
                }
            } else if (k == SDLK_ESCAPE) {
                mode = Mode::Menu;
                Beeper::sfx_pause_menu();
            }
            if (pending.x != pending_prev.x || pending.y != pending_prev.y) {
                Beeper::sfx_turn();
            }
        }

        const Uint32 now = SDL_GetTicks();

        if (mode == Mode::Playing && now - last_move >= move_ms) {
            last_move = now;
            dir = pending;

            Point head = snake.front();
            head.x += dir.x;
            head.y += dir.y;

            if (head.x < 0 || head.x >= GRID_W || head.y < 0 || head.y >= GRID_H) {
                mode = Mode::GameOver;
                Beeper::sfx_die();
            } else if (cell_occupied(snake, head.x, head.y)) {
                mode = Mode::GameOver;
                Beeper::sfx_die();
            } else {
                snake.push_front(head);
                if (head.x == food.x && head.y == food.y) {
                    ++score;
                    spawn_food(food, snake);
                    move_ms = std::max(MOVE_MS_MIN, move_ms - 3);
                    Beeper::sfx_eat(score);
                } else {
                    snake.pop_back();
                    Beeper::sfx_move();
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, nokiaGreen.r, nokiaGreen.g, nokiaGreen.b, 255);
        SDL_RenderClear(renderer);

        SDL_Rect hud = {0, 0, SCREEN_WIDTH, HUD_H};
        SDL_SetRenderDrawColor(renderer, nokiaDark.r, nokiaDark.g, nokiaDark.b, 255);
        SDL_RenderFillRect(renderer, &hud);

        if (font) {
            draw_text(renderer, font, "Score: " + std::to_string(score), 10, 6, nokiaGreen,
                      &nokiaDark);
        }

        if (mode == Mode::Menu) {
            if (fontBig) {
                int tw = 0;
                TTF_SizeText(fontBig, "AC Holding Snake", &tw, nullptr);
                draw_text(renderer, fontBig, "AC Holding Snake", (SCREEN_WIDTH - tw) / 2, 120,
                          nokiaDark, nullptr);
            }
            if (font) {
                const char* sub = "ENTER / SPACE — start   ESC — quit window";
                int sw = 0;
                TTF_SizeText(font, sub, &sw, nullptr);
                draw_text(renderer, font, sub, (SCREEN_WIDTH - sw) / 2, 200, nokiaDark, nullptr);
                const char* move = "Arrows or WASD  |  Famicom-style bleeps";
                int mw = 0;
                TTF_SizeText(font, move, &mw, nullptr);
                draw_text(renderer, font, move, (SCREEN_WIDTH - mw) / 2, 235, nokiaDark,
                          nullptr);
            }
        } else if (mode == Mode::Playing || mode == Mode::GameOver) {
            for (const Point& p : snake) {
                fill_cell(renderer, p.x, p.y, nokiaDark);
            }
            fill_cell(renderer, food.x, food.y, foodColor);

            if (mode == Mode::GameOver) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 15, 56, 15, 200);
                SDL_Rect overlay = {40, 100, SCREEN_WIDTH - 80, 160};
                SDL_RenderFillRect(renderer, &overlay);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

                if (fontBig) {
                    const char* go = "GAME OVER";
                    int gw = 0;
                    TTF_SizeText(fontBig, go, &gw, nullptr);
                    draw_text(renderer, fontBig, go, (SCREEN_WIDTH - gw) / 2, 115, nokiaGreen,
                              nullptr);
                }
                if (font) {
                    std::string line = "Score: " + std::to_string(score);
                    int w1 = 0;
                    TTF_SizeText(font, line.c_str(), &w1, nullptr);
                    draw_text(renderer, font, line, (SCREEN_WIDTH - w1) / 2, 155, nokiaGreen,
                              nullptr);
                    const char* hint = "ENTER — play again   ESC — menu";
                    int w2 = 0;
                    TTF_SizeText(font, hint, &w2, nullptr);
                    draw_text(renderer, font, hint, (SCREEN_WIDTH - w2) / 2, 190, nokiaGreen,
                              nullptr);
                }
            }
        }

        SDL_RenderPresent(renderer);

        Uint32 frameTime = SDL_GetTicks() - frameStart;
        if (FRAME_DELAY > frameTime) {
            SDL_Delay(FRAME_DELAY - frameTime);
        }
    }

    if (fontBig) {
        TTF_CloseFont(fontBig);
    }
    if (font) {
        TTF_CloseFont(font);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    Beeper::shutdown();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
