// Оболочка эмулятора: окно SDL2, библиотека ромов, настройки, ввод, вывод изображения и звука.
// Вся эмуляция живёт в библиотеке gbcore.
//
// Экраны:
//   Библиотека  - карточки игр из выбранной папки. Стрелки/мышь = выбор, Enter/щелчок = запуск,
//                 F2 = выбрать папку, F3 = настройки, F5 = обновить список, Esc = выход.
//   Настройки   - переназначение кнопок, громкость, папка с ромами.
//   Игра        - Esc = пауза (меню), F11 = полный экран, F12 = снимок экрана.
// Можно перетащить в окно файл .gb/.gbc (запуск) или папку (станет папкой с ромами).
//
// Параметры командной строки: GBCEmu.exe [rom.gb] [--dmg]
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "Audio.h"
#include "Cpu.h"
#include "Platform.h"
#include "RomLibrary.h"
#include "Settings.h"
#include "Ui.h"

namespace fs = std::filesystem;

namespace {

constexpr int kW = 640, kH = 576;  // размер холста интерфейса (4 x экран Game Boy)
constexpr int kFontPx = 16;
constexpr const char* kAppName = "GBC EMU";

// Порядок кнопок в меню настроек (номера битов gb::Button) и их подписи (по номеру бита)
constexpr int kMenuButtons[8] = {2, 3, 1, 0, 4, 5, 7, 6};
const char* const kButtonLabels[8] = {"Вправо", "Влево", "Вверх", "Вниз", "A", "B", "Select", "Start"};

constexpr int kSetVolume = 8, kSetFolder = 9, kSetResetKeys = 10, kSetBack = 11, kSetCount = 12;

const char* const kPauseItems[] = {"Продолжить", "Перезапустить игру", "Настройки", "Выйти в библиотеку"};
constexpr int kPauseCount = 4;

// Сеанс игры. Шина держит указатель на картридж, а процессор ссылку на шину,
// поэтому объект живёт в куче и никуда не перемещается.
struct Game {
    Cartridge cart;
    gb::Bus bus;
    std::unique_ptr<gb::Cpu> cpu;
    fs::path path;
    std::string title;
    bool crashed = false;
    std::string crashText;
    uint8_t ignoreHeld = 0;  // кнопки, зажатые в момент запуска/возврата из меню: не нажимаются, пока их не отпустят
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum class Screen { Library, Settings, Game, Pause };

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string keyName(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_RETURN) return "Enter";
    const char* n = SDL_GetScancodeName(sc);
    return (n && *n) ? n : fmt("#%d", static_cast<int>(sc));
}

}  // namespace

class App {
public:
    int run(int argc, char** argv);

private:
    // --- окно и ресурсы ---
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* canvas_ = nullptr;   // холст интерфейса 640x576
    SDL_Texture* gameTex_ = nullptr;  // кадр Game Boy 160x144
    Ui ui_;
    AudioOut audio_;
    Settings settings_;
    fs::path configPath_;
    bool running_ = true;
    bool resetTiming_ = true;
    SDL_Rect canvasDst_{0, 0, kW, kH};

    // --- состояние ---
    Screen screen_ = Screen::Library;
    Screen settingsReturn_ = Screen::Library;
    std::vector<RomEntry> roms_;
    std::string scanError_;
    std::unique_ptr<Game> game_;
    bool forceDmg_ = false;

    int sel_ = 0, scroll_ = 0;          // библиотека: выбранная карточка и первая видимая строка
    int hoverButton_ = -1;              // библиотека: кнопка панели под курсором
    int setSel_ = 0;                    // настройки
    bool waitingKey_ = false;
    int pauseSel_ = 0;
    std::string toast_;
    Uint32 toastUntil_ = 0;

    // Отладка: --shot файл.bmp [--screen settings|pause] сохраняет снимок холста и выходит
    fs::path shotPath_;
    std::string shotScreen_;
    int frameNo_ = 0;
    int shotFrame_ = 70;  // кадр, на котором делается снимок (--shot-frame N)

    // --- раскладка библиотеки ---
    static constexpr int kCols = 3, kCardW = 192, kGap = 12, kGridX = 20, kGridY = 132, kGridBottom = 524;
    int cardH() const { return 30 + 3 * ui_.lineHeight(); }
    int rowPitch() const { return cardH() + kGap; }
    int visibleRows() const { return std::max(1, (kGridBottom - kGridY + kGap) / rowPitch()); }
    int totalRows() const { return (static_cast<int>(roms_.size()) + kCols - 1) / kCols; }
    Rect cardRect(int i) const {
        const int row = i / kCols - scroll_, col = i % kCols;
        return {kGridX + col * (kCardW + kGap), kGridY + row * rowPitch(), kCardW, cardH()};
    }
    std::vector<Rect> toolbarRects();
    static constexpr const char* kToolbar[3] = {"F2 Папка", "F3 Настройки", "F5 Обновить"};

    Rect settingsRow(int i) const { return {24, 78 + i * 34, kW - 48, 30}; }
    Rect pauseRow(int i) const { return {150, 232 + i * 40, kW - 300, 32}; }

    // --- работа ---
    bool initWindow();
    void shutdown();
    void loadSettings();
    void saveSettings();
    void rescan();
    void toast(const std::string& msg, Uint32 ms = 3000) { toast_ = msg; toastUntil_ = SDL_GetTicks() + ms; }
    bool toastActive() const { return !toast_.empty() && !SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_); }

    bool startGame(const fs::path& path);
    void stopGame();
    void openPause();
    void resumeGame();
    void openSettings(Screen returnTo);
    void chooseFolder();
    void setRomDir(const fs::path& dir);
    void updateTitle();
    void latchHeldButtons();

    void handleEvent(const SDL_Event& e);
    void handleKey(const SDL_KeyboardEvent& k);
    void libraryKey(SDL_Scancode sc, bool repeat);
    void settingsKey(SDL_Scancode sc, bool repeat);
    void pauseKey(SDL_Scancode sc, bool repeat);
    void activateSetting(int i);
    void activatePause(int i);
    void ensureVisible();
    bool toCanvas(int wx, int wy, int& cx, int& cy) const;
    void mouseMove(int cx, int cy);
    void mouseClick(int cx, int cy);

    void emulateFrame();
    void render();
    void drawLibrary();
    void drawCard(int i);
    void drawSettings();
    void drawPause();
    void drawGameToCanvas();
    void drawGameToWindow();
    void saveScreenshot();
    void saveCanvasShot();
};

// ================= запуск =================

int App::run(int argc, char** argv) {
    fs::path romArg;
    const std::vector<fs::path> args = platform::commandLineArgs(argc, argv);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string a = platform::toUtf8(args[i]);
        if (a == "--dmg") forceDmg_ = true;
        else if (a == "--shot" && i + 1 < args.size()) shotPath_ = args[++i];
        else if (a == "--screen" && i + 1 < args.size()) shotScreen_ = platform::toUtf8(args[++i]);
        else if (a == "--shot-frame" && i + 1 < args.size()) shotFrame_ = std::max(20, std::atoi(platform::toUtf8(args[++i]).c_str()));
        else if (!a.empty() && a[0] != '-') romArg = args[i];
    }

    loadSettings();
    if (!initWindow()) return 1;
    rescan();
    if (!romArg.empty()) startGame(romArg);
    if (!audio_.ok()) toast("Звуковое устройство недоступно: игра пойдёт без звука", 5000);

    const double ticksPerSecond = static_cast<double>(SDL_GetPerformanceFrequency());
    const double frameSeconds = static_cast<double>(gb::kDotsPerFrame) / gb::kClockHz;  // 16,74 мс
    uint64_t nextFrame = SDL_GetPerformanceCounter();

    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        if (!running_) break;

        if (screen_ == Screen::Game && game_) emulateFrame();
        render();
        ++frameNo_;

        if (!shotPath_.empty()) {  // режим снимка для проверки интерфейса
            if (frameNo_ == shotFrame_ - 10) {
                if (shotScreen_ == "settings") openSettings(screen_ == Screen::Game ? Screen::Pause : Screen::Library);
                else if (shotScreen_ == "pause" && game_) openPause();
            }
            if (frameNo_ >= shotFrame_) running_ = false;
        }

        // Выдерживаем 59,73 кадра в секунду (как настоящий Game Boy)
        if (resetTiming_) {
            nextFrame = SDL_GetPerformanceCounter();
            resetTiming_ = false;
        }
        nextFrame += static_cast<uint64_t>(frameSeconds * ticksPerSecond);
        const uint64_t now = SDL_GetPerformanceCounter();
        if (nextFrame > now) {
            const double wait = static_cast<double>(nextFrame - now) / ticksPerSecond;
            if (wait > 0.002) SDL_Delay(static_cast<Uint32>((wait - 0.001) * 1000.0));
            while (SDL_GetPerformanceCounter() < nextFrame) {}  // остаток добираем точно
        } else if (now - nextFrame > static_cast<uint64_t>(0.1 * ticksPerSecond)) {
            nextFrame = now;  // сильно отстали (окно перетаскивали и т.п.): не пытаемся наверстать
        }
    }

    stopGame();
    saveSettings();
    shutdown();
    return 0;
}

bool App::initWindow() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, SDL_GetError(), nullptr);
        return false;
    }

    // Окно: наибольший целый множитель 640x576, который помещается на экране
    int mul = 1;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0)
        mul = std::max(1, std::min((usable.w - 40) / kW, (usable.h - 80) / kH));
    win_ = SDL_CreateWindow(kAppName, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW * mul, kH * mul,
                            SDL_WINDOW_RESIZABLE);
    if (!win_) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, SDL_GetError(), nullptr);
        return false;
    }
    SDL_SetWindowMinimumSize(win_, kW / 2, kH / 2);

    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
    if (!ren_) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, SDL_GetError(), win_);
        return false;
    }

    canvas_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (canvas_) SDL_SetTextureScaleMode(canvas_, SDL_ScaleModeNearest);
    else SDL_RenderSetLogicalSize(ren_, kW, kH);  // запасной путь: рисуем прямо в окно
    gameTex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, gb::kScreenW, gb::kScreenH);
    if (!gameTex_) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kAppName, SDL_GetError(), win_);
        return false;
    }
    SDL_SetTextureScaleMode(gameTex_, SDL_ScaleModeNearest);

    ui_.init(ren_, kFontPx);
    audio_.open();
    return true;
}

void App::shutdown() {
    audio_.close();
    ui_.shutdown();
    if (gameTex_) SDL_DestroyTexture(gameTex_);
    if (canvas_) SDL_DestroyTexture(canvas_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    SDL_Quit();
}

// ================= настройки и библиотека =================

void App::loadSettings() {
    const fs::path exe = platform::exeDir();
    configPath_ = exe / "gbcemu.ini";
    std::error_code ec;
    bool loaded = settings_.load(configPath_);
    if (!loaded) {
        // Рядом с exe файла нет: возможно, он лежит в профиле пользователя (если папка exe только для чтения)
        if (char* pref = SDL_GetPrefPath("GBCEmu", "GBCEmu")) {
            const fs::path alt = platform::fromUtf8(pref) / "gbcemu.ini";
            SDL_free(pref);
            if (fs::exists(alt, ec) && settings_.load(alt)) {
                configPath_ = alt;
                loaded = true;
            }
        }
    }
    if (settings_.romDir.empty()) {
        settings_.romDir = exe / "roms";
        fs::create_directories(settings_.romDir, ec);
    }
    if (!loaded) saveSettings();
}

void App::saveSettings() {
    if (settings_.save(configPath_)) return;
    if (char* pref = SDL_GetPrefPath("GBCEmu", "GBCEmu")) {  // папка exe недоступна для записи
        configPath_ = platform::fromUtf8(pref) / "gbcemu.ini";
        SDL_free(pref);
        settings_.save(configPath_);
    }
}

void App::rescan() {
    roms_ = scanRoms(settings_.romDir, scanError_);
    sel_ = std::clamp(sel_, 0, std::max(0, static_cast<int>(roms_.size()) - 1));
    scroll_ = 0;
    ensureVisible();
}

void App::setRomDir(const fs::path& dir) {
    settings_.romDir = dir;
    saveSettings();
    sel_ = 0;
    rescan();
    toast(roms_.empty() ? "В этой папке ромы не найдены" : fmt("Найдено игр: %d", static_cast<int>(roms_.size())));
}

void App::chooseFolder() {
    if (auto dir = platform::pickFolder(win_, settings_.romDir)) setRomDir(*dir);
    resetTiming_ = true;  // диалог блокировал цикл
}

std::vector<Rect> App::toolbarRects() {
    std::vector<Rect> r;
    int x = kGridX;
    for (const char* label : kToolbar) {
        const int w = ui_.textWidth(label) + 24;
        r.push_back({x, 64, w, ui_.lineHeight() + 10});
        x += w + 14;
    }
    return r;
}

// ================= игра =================

bool App::startGame(const fs::path& path) {
    // Сначала сохраняем и закрываем прежнюю игру: при перезапуске той же игры
    // новый картридж должен прочитать уже обновлённый файл .sav
    stopGame();
    auto g = std::make_unique<Game>();
    if (!g->cart.load(path)) {
        if (screen_ != Screen::Settings) screen_ = Screen::Library;
        toast("Не удалось загрузить ROM: " + platform::toUtf8(path.filename()));
        return false;
    }

    g->path = path;
    g->title = g->cart.title().empty() ? platform::toUtf8(path.stem()) : g->cart.title();
    for (const RomEntry& r : roms_) if (r.path == path) g->title = r.title;  // то же название, что на карточке
    g->bus.attach(&g->cart);
    g->bus.cgbMode = g->cart.cgbSupported() && !forceDmg_;  // игры без поддержки CGB идут в режиме DMG
    g->bus.apu().setSampleRate(audio_.freq());
    g->cpu = std::make_unique<gb::Cpu>(g->bus);
    game_ = std::move(g);

    screen_ = Screen::Game;
    latchHeldButtons();
    audio_.start();
    resetTiming_ = true;
    updateTitle();
    if (!game_->cart.supported()) toast("Тип картриджа не поддерживается: игра может работать неправильно", 5000);
    return true;
}

void App::stopGame() {
    if (!game_) return;
    game_->cart.saveRam();  // батарейная RAM и часы сохраняются в .sav рядом с ROM
    game_.reset();
    audio_.stop();
    updateTitle();
}

void App::openPause() {
    if (!game_) return;
    screen_ = Screen::Pause;
    pauseSel_ = 0;
    audio_.stop();
    game_->cart.saveRam();  // на случай, если окно закроют из меню
}

void App::resumeGame() {
    if (!game_) { screen_ = Screen::Library; return; }
    screen_ = Screen::Game;
    latchHeldButtons();
    audio_.start();
    resetTiming_ = true;
}

void App::openSettings(Screen returnTo) {
    settingsReturn_ = returnTo;
    screen_ = Screen::Settings;
    setSel_ = 0;
    waitingKey_ = false;
}

void App::updateTitle() {
    const std::string t = game_ ? std::string(kAppName) + " — " + game_->title : std::string(kAppName);
    SDL_SetWindowTitle(win_, t.c_str());
}

void App::latchHeldButtons() {
    if (!game_) return;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint8_t held = 0;
    for (int i = 0; i < 8; ++i) if (ks[settings_.keys[i]]) held |= static_cast<uint8_t>(1 << i);
    game_->ignoreHeld = held;
}

void App::emulateFrame() {
    Game& g = *game_;

    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint8_t pressed = 0;
    for (int i = 0; i < 8; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1 << i);
        if (ks[settings_.keys[i]]) {
            if (!(g.ignoreHeld & bit)) pressed |= bit;
        } else {
            g.ignoreHeld &= static_cast<uint8_t>(~bit);
        }
    }
    // Одновременно нажатые противоположные направления на настоящей крестовине невозможны
    if ((pressed & gb::kLeft) && (pressed & gb::kRight)) pressed &= static_cast<uint8_t>(~(gb::kLeft | gb::kRight));
    if ((pressed & gb::kUp) && (pressed & gb::kDown)) pressed &= static_cast<uint8_t>(~(gb::kUp | gb::kDown));
    g.bus.setButtons(pressed);

    gb::Apu& apu = g.bus.apu();
    apu.setVolume(settings_.volume / 100.f);
    if (audio_.ok()) apu.setSampleRate(audio_.suggestedRate());

    // Один кадр времени = 70224 точки PPU, даже если экран выключен
    if (!g.crashed) {
        const uint64_t start = g.bus.ppuDots();
        while (g.bus.ppuDots() - start < gb::kDotsPerFrame) {
            if (g.cpu->step() == 0) {
                g.crashed = true;
                g.crashText = fmt("Эмуляция остановлена: неизвестный опкод 0x%02X по адресу 0x%04X",
                                  g.cpu->lastUnknownOpcode(), static_cast<unsigned>((g.cpu->regs().pc - 1) & 0xFFFF));
                break;
            }
        }
    }

    std::vector<int16_t>& s = apu.samples();
    if (audio_.ok() && !g.crashed) audio_.push(s.data(), s.size());
    s.clear();
}

// ================= ввод =================

bool App::toCanvas(int wx, int wy, int& cx, int& cy) const {
    if (!canvas_) { cx = wx; cy = wy; return true; }  // логический размер SDL уже пересчитал координаты
    int ww = 0, wh = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(win_, &ww, &wh);
    SDL_GetRendererOutputSize(ren_, &ow, &oh);
    if (ww <= 0 || wh <= 0 || canvasDst_.w <= 0 || canvasDst_.h <= 0) return false;
    const double px = wx * static_cast<double>(ow) / ww, py = wy * static_cast<double>(oh) / wh;
    cx = static_cast<int>(std::floor((px - canvasDst_.x) * kW / canvasDst_.w));
    cy = static_cast<int>(std::floor((py - canvasDst_.y) * kH / canvasDst_.h));
    return cx >= 0 && cy >= 0 && cx < kW && cy < kH;
}

void App::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT:
            running_ = false;
            break;
        case SDL_KEYDOWN:
            handleKey(e.key);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && screen_ == Screen::Game && shotPath_.empty()) openPause();
            break;
        case SDL_MOUSEMOTION: {
            int cx, cy;
            if (screen_ != Screen::Game && toCanvas(e.motion.x, e.motion.y, cx, cy)) mouseMove(cx, cy);
            else hoverButton_ = -1;
            break;
        }
        case SDL_MOUSEBUTTONDOWN: {
            int cx, cy;
            if (e.button.button == SDL_BUTTON_LEFT && screen_ != Screen::Game && toCanvas(e.button.x, e.button.y, cx, cy))
                mouseClick(cx, cy);
            break;
        }
        case SDL_MOUSEWHEEL:
            if (screen_ == Screen::Library && e.wheel.y != 0) {
                const int maxScroll = std::max(0, totalRows() - visibleRows());
                scroll_ = std::clamp(scroll_ - (e.wheel.y > 0 ? 1 : -1), 0, maxScroll);
            }
            break;
        case SDL_DROPFILE: {
            const fs::path p = platform::fromUtf8(e.drop.file ? e.drop.file : "");
            SDL_free(e.drop.file);
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                if (screen_ == Screen::Game) openPause();
                setRomDir(p);
            } else if (hasRomExtension(p)) {
                startGame(p);
            } else {
                toast("Это не ROM: нужен файл .gb или .gbc");
            }
            break;
        }
        case SDL_RENDER_DEVICE_RESET:  // устройство пересоздано: все текстуры потеряны
            ui_.dropTextures();
            if (canvas_) SDL_DestroyTexture(canvas_);
            if (gameTex_) SDL_DestroyTexture(gameTex_);
            canvas_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
            if (canvas_) SDL_SetTextureScaleMode(canvas_, SDL_ScaleModeNearest);
            else SDL_RenderSetLogicalSize(ren_, kW, kH);
            gameTex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, gb::kScreenW, gb::kScreenH);
            if (gameTex_) SDL_SetTextureScaleMode(gameTex_, SDL_ScaleModeNearest);
            break;
        default:
            break;
    }
}

void App::handleKey(const SDL_KeyboardEvent& k) {
    const SDL_Scancode sc = k.keysym.scancode;
    const bool repeat = k.repeat != 0;

    // Ожидание новой клавиши в настройках перехватывает всё
    if (screen_ == Screen::Settings && waitingKey_) {
        if (repeat) return;
        waitingKey_ = false;
        if (sc == SDL_SCANCODE_ESCAPE) return;
        if (sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12) {
            toast("F11 и F12 заняты (полный экран и снимок экрана)");
            return;
        }
        const int bit = kMenuButtons[setSel_];
        for (int i = 0; i < 8; ++i)  // клавиша уже занята другой кнопкой: меняем их местами
            if (i != bit && settings_.keys[i] == sc) settings_.keys[i] = settings_.keys[bit];
        settings_.keys[bit] = sc;
        saveSettings();
        return;
    }

    if (sc == SDL_SCANCODE_F11 && !repeat) {
        const bool full = (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_SetWindowFullscreen(win_, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        resetTiming_ = true;
        return;
    }

    switch (screen_) {
        case Screen::Library: libraryKey(sc, repeat); break;
        case Screen::Settings: settingsKey(sc, repeat); break;
        case Screen::Pause: pauseKey(sc, repeat); break;
        case Screen::Game:
            if (repeat) break;
            if (sc == SDL_SCANCODE_ESCAPE) openPause();
            else if (sc == SDL_SCANCODE_F12) saveScreenshot();
            break;
    }
}

void App::ensureVisible() {
    if (roms_.empty()) { scroll_ = 0; return; }
    const int row = sel_ / kCols;
    if (row < scroll_) scroll_ = row;
    if (row >= scroll_ + visibleRows()) scroll_ = row - visibleRows() + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, totalRows() - visibleRows()));
}

void App::libraryKey(SDL_Scancode sc, bool repeat) {
    const int n = static_cast<int>(roms_.size());
    switch (sc) {
        case SDL_SCANCODE_LEFT:  if (sel_ > 0) --sel_; break;
        case SDL_SCANCODE_RIGHT: if (sel_ + 1 < n) ++sel_; break;
        case SDL_SCANCODE_UP:    if (sel_ - kCols >= 0) sel_ -= kCols; break;
        case SDL_SCANCODE_DOWN:
            if (sel_ + kCols < n) sel_ += kCols;
            else if (n > 0 && sel_ / kCols < (n - 1) / kCols) sel_ = n - 1;  // в неполную последнюю строку
            break;
        case SDL_SCANCODE_HOME: sel_ = 0; break;
        case SDL_SCANCODE_END: sel_ = std::max(0, n - 1); break;
        case SDL_SCANCODE_PAGEUP: sel_ = std::max(0, sel_ - kCols * visibleRows()); break;
        case SDL_SCANCODE_PAGEDOWN: sel_ = std::max(0, std::min(n - 1, sel_ + kCols * visibleRows())); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!repeat && n > 0) startGame(roms_[sel_].path);
            return;
        case SDL_SCANCODE_F2: if (!repeat) chooseFolder(); return;
        case SDL_SCANCODE_F3: if (!repeat) openSettings(Screen::Library); return;
        case SDL_SCANCODE_F5:
            if (!repeat) {
                rescan();
                toast(fmt("Список обновлён: %d", static_cast<int>(roms_.size())));
            }
            return;
        case SDL_SCANCODE_ESCAPE: if (!repeat) running_ = false; return;
        default: return;
    }
    ensureVisible();
}

void App::settingsKey(SDL_Scancode sc, bool repeat) {
    switch (sc) {
        case SDL_SCANCODE_UP: setSel_ = (setSel_ + kSetCount - 1) % kSetCount; break;
        case SDL_SCANCODE_DOWN: setSel_ = (setSel_ + 1) % kSetCount; break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            if (setSel_ == kSetVolume) {
                settings_.volume = std::clamp(settings_.volume + (sc == SDL_SCANCODE_RIGHT ? 10 : -10), 0, 100);
                saveSettings();
            }
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!repeat) activateSetting(setSel_);
            break;
        case SDL_SCANCODE_ESCAPE:
            if (!repeat) activateSetting(kSetBack);
            break;
        default: break;
    }
}

void App::activateSetting(int i) {
    if (i < 8) {
        waitingKey_ = true;
    } else if (i == kSetVolume) {
        settings_.volume = settings_.volume >= 100 ? 0 : settings_.volume + 10;
        saveSettings();
    } else if (i == kSetFolder) {
        chooseFolder();
    } else if (i == kSetResetKeys) {
        settings_.keys = Settings::defaultKeys();
        saveSettings();
        toast("Клавиши сброшены по умолчанию");
    } else if (i == kSetBack) {
        waitingKey_ = false;
        screen_ = (settingsReturn_ == Screen::Pause && game_) ? Screen::Pause : Screen::Library;
    }
}

void App::pauseKey(SDL_Scancode sc, bool repeat) {
    switch (sc) {
        case SDL_SCANCODE_UP: pauseSel_ = (pauseSel_ + kPauseCount - 1) % kPauseCount; break;
        case SDL_SCANCODE_DOWN: pauseSel_ = (pauseSel_ + 1) % kPauseCount; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!repeat) activatePause(pauseSel_);
            break;
        case SDL_SCANCODE_ESCAPE:
            if (!repeat) resumeGame();
            break;
        default: break;
    }
}

void App::activatePause(int i) {
    switch (i) {
        case 0: resumeGame(); break;
        case 1: if (game_) { const fs::path p = game_->path; startGame(p); } break;
        case 2: openSettings(Screen::Pause); break;
        default: stopGame(); screen_ = Screen::Library; break;
    }
}

void App::mouseMove(int cx, int cy) {
    hoverButton_ = -1;
    if (screen_ == Screen::Library) {
        const std::vector<Rect> tb = toolbarRects();
        for (int i = 0; i < static_cast<int>(tb.size()); ++i) if (tb[i].contains(cx, cy)) hoverButton_ = i;
        const int first = scroll_ * kCols;
        const int last = std::min(static_cast<int>(roms_.size()), (scroll_ + visibleRows()) * kCols);
        for (int i = first; i < last; ++i) if (cardRect(i).contains(cx, cy)) sel_ = i;
    } else if (screen_ == Screen::Settings && !waitingKey_) {
        for (int i = 0; i < kSetCount; ++i) if (settingsRow(i).contains(cx, cy)) setSel_ = i;
    } else if (screen_ == Screen::Pause) {
        for (int i = 0; i < kPauseCount; ++i) if (pauseRow(i).contains(cx, cy)) pauseSel_ = i;
    }
}

void App::mouseClick(int cx, int cy) {
    if (screen_ == Screen::Library) {
        const std::vector<Rect> tb = toolbarRects();
        if (tb[0].contains(cx, cy)) { chooseFolder(); return; }
        if (tb[1].contains(cx, cy)) { openSettings(Screen::Library); return; }
        if (tb[2].contains(cx, cy)) { rescan(); toast(fmt("Список обновлён: %d", static_cast<int>(roms_.size()))); return; }
        const int first = scroll_ * kCols;
        const int last = std::min(static_cast<int>(roms_.size()), (scroll_ + visibleRows()) * kCols);
        for (int i = first; i < last; ++i) {
            if (cardRect(i).contains(cx, cy)) {
                sel_ = i;
                startGame(roms_[i].path);
                return;
            }
        }
    } else if (screen_ == Screen::Settings) {
        if (waitingKey_) { waitingKey_ = false; return; }  // щелчок отменяет ожидание клавиши
        for (int i = 0; i < kSetCount; ++i) {
            if (!settingsRow(i).contains(cx, cy)) continue;
            setSel_ = i;
            if (i == kSetVolume) {  // левая половина значения уменьшает, правая увеличивает
                const Rect r = settingsRow(i);
                const int mid = r.x + 300 + 60;
                settings_.volume = std::clamp(settings_.volume + (cx < mid ? -10 : 10), 0, 100);
                saveSettings();
            } else {
                activateSetting(i);
            }
            return;
        }
    } else if (screen_ == Screen::Pause) {
        for (int i = 0; i < kPauseCount; ++i) {
            if (pauseRow(i).contains(cx, cy)) { activatePause(i); return; }
        }
    }
}

// ================= отрисовка =================

void App::render() {
    const bool direct = screen_ == Screen::Game && game_;  // игра рисуется прямо в окно с целым масштабом
    if (game_) SDL_UpdateTexture(gameTex_, nullptr, game_->bus.ppu().frame().data(), gb::kScreenW * 4);

    if (direct) {
        SDL_SetRenderTarget(ren_, nullptr);
        if (!canvas_) SDL_RenderSetLogicalSize(ren_, 0, 0);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);
        drawGameToWindow();
        if (!shotPath_.empty() && frameNo_ == shotFrame_ - 1) saveScreenshot();
        SDL_RenderPresent(ren_);
        return;
    }

    if (canvas_) {
        SDL_SetRenderTarget(ren_, canvas_);
    } else {
        SDL_RenderSetLogicalSize(ren_, kW, kH);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);
    }
    ui_.fill(0, 0, kW, kH, pal::kBg);
    switch (screen_) {
        case Screen::Library: drawLibrary(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Pause: drawPause(); break;
        case Screen::Game: break;
    }
    if (!shotPath_.empty() && frameNo_ == shotFrame_ - 1) saveCanvasShot();

    if (canvas_) {
        SDL_SetRenderTarget(ren_, nullptr);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        SDL_RenderClear(ren_);
        int ow = kW, oh = kH;
        SDL_GetRendererOutputSize(ren_, &ow, &oh);
        double s = std::min(static_cast<double>(ow) / kW, static_cast<double>(oh) / kH);
        if (s >= 1.0 && std::floor(s) / s >= 0.8) s = std::floor(s);  // целый масштаб, если он не слишком мельчит
        const int w = std::max(1, static_cast<int>(kW * s)), h = std::max(1, static_cast<int>(kH * s));
        canvasDst_ = {(ow - w) / 2, (oh - h) / 2, w, h};
        SDL_RenderCopy(ren_, canvas_, nullptr, &canvasDst_);
    }
    SDL_RenderPresent(ren_);
}

void App::drawGameToWindow() {
    int ow = kW, oh = kH;
    SDL_GetRendererOutputSize(ren_, &ow, &oh);
    const int k = std::min(ow / gb::kScreenW, oh / gb::kScreenH);
    int w, h;
    if (k >= 1) { w = gb::kScreenW * k; h = gb::kScreenH * k; }
    else {
        const double s = std::min(static_cast<double>(ow) / gb::kScreenW, static_cast<double>(oh) / gb::kScreenH);
        w = std::max(1, static_cast<int>(gb::kScreenW * s));
        h = std::max(1, static_cast<int>(gb::kScreenH * s));
    }
    const SDL_Rect dst{(ow - w) / 2, (oh - h) / 2, w, h};
    SDL_RenderCopy(ren_, gameTex_, nullptr, &dst);

    // Сообщение поверх игры (ошибка эмуляции или уведомление)
    std::string msg;
    Color color = pal::kLight;
    if (game_->crashed) { msg = game_->crashText + " · Esc = меню"; color = pal::kWarn; }
    else if (toastActive()) msg = toast_;
    if (!msg.empty()) {
        const int ts = std::max(1, static_cast<int>(std::lround(h / static_cast<double>(kH))));
        const int barH = ui_.lineHeight(ts) + 12 * ts;
        ui_.fill(dst.x, dst.y + h - barH, w, barH, {0, 0, 0, 200});
        ui_.text(dst.x + 10 * ts, dst.y + h - barH + 6 * ts, ui_.fit(msg, (w - 20 * ts) / ts), color, ts);
    }
}

void App::drawGameToCanvas() {
    const SDL_Rect dst{0, 0, kW, kH};
    SDL_RenderCopy(ren_, gameTex_, nullptr, &dst);
}

void App::drawLibrary() {
    const int lh = ui_.lineHeight();

    // Заголовок
    ui_.text(kGridX + 3, 17, kAppName, pal::kShadow, 2);
    ui_.text(kGridX, 14, kAppName, pal::kAccent, 2);
    const int titleW = ui_.textWidth(kAppName, 2);
    ui_.fill(kGridX + titleW + 14, 14 + lh - 4, 10, 10, pal::kGbc);  // "светодиод питания"
    const std::string count = fmt("Игр: %d", static_cast<int>(roms_.size()));
    ui_.text(kW - kGridX - ui_.textWidth(count), 14 + lh / 2, count, pal::kDim);

    // Панель кнопок
    const std::vector<Rect> tb = toolbarRects();
    for (int i = 0; i < static_cast<int>(tb.size()); ++i) {
        const bool hot = i == hoverButton_;
        ui_.box(tb[i].x, tb[i].y, tb[i].w, tb[i].h, hot ? pal::kAccent : pal::kPanel, hot ? pal::kLight : pal::kAccent, 3);
        ui_.text(tb[i].x + 12, tb[i].y + 5, kToolbar[i], hot ? pal::kBg : pal::kLight);
    }

    // Папка
    const std::string label = "Папка: ";
    const int lw = ui_.textWidth(label);
    ui_.text(kGridX, 102, label, pal::kDim);
    ui_.text(kGridX + lw, 102, ui_.fit(platform::toUtf8(settings_.romDir), kW - 2 * kGridX - lw, true), pal::kLight);

    if (roms_.empty()) {
        const int cy = 250;
        ui_.box(60, cy - 30, kW - 120, 190, pal::kPanel, pal::kAccent, 4);
        ui_.textCentered(kW / 2, cy, scanError_.empty() ? "Ромы не найдены" : scanError_, pal::kWarn);
        ui_.textCentered(kW / 2, cy + lh * 2, "Положите файлы .gb / .gbc в эту папку", pal::kLight);
        ui_.textCentered(kW / 2, cy + lh * 3, "и нажмите F5, или выберите другую: F2.", pal::kLight);
        ui_.textCentered(kW / 2, cy + lh * 5, "ROM можно просто перетащить в окно.", pal::kDim);
    } else {
        const int first = scroll_ * kCols;
        const int last = std::min(static_cast<int>(roms_.size()), (scroll_ + visibleRows()) * kCols);
        for (int i = first; i < last; ++i) drawCard(i);

        // Полоса прокрутки
        if (totalRows() > visibleRows()) {
            const int trackY = kGridY, trackH = visibleRows() * rowPitch() - kGap;
            const int thumbH = std::max(16, trackH * visibleRows() / totalRows());
            const int maxScroll = totalRows() - visibleRows();
            const int thumbY = trackY + (trackH - thumbH) * scroll_ / maxScroll;
            ui_.fill(kW - 10, trackY, 4, trackH, pal::kShadow);
            ui_.fill(kW - 10, thumbY, 4, thumbH, pal::kAccent);
        }
    }

    // Строка состояния: уведомление или сведения о выбранной игре
    if (toastActive()) {
        ui_.text(kGridX, 528, ui_.fit(toast_, kW - 2 * kGridX), pal::kWarn);
    } else if (!roms_.empty()) {
        const RomEntry& r = roms_[sel_];
        const std::string info = fmt("%s · %s · %llu КБ", r.fileName.c_str(), r.typeName.c_str(),
                                     static_cast<unsigned long long>(r.size / 1024));
        ui_.text(kGridX, 528, ui_.fit(info, kW - 2 * kGridX), pal::kDim);
    }
    ui_.fill(kGridX, 550, kW - 2 * kGridX, 2, pal::kPanel);
    ui_.text(kGridX, 554, "Стрелки: выбор   Enter: играть   Esc: выход", pal::kDim);
}

void App::drawCard(int i) {
    const RomEntry& r = roms_[i];
    const Rect rc = cardRect(i);
    const bool s = i == sel_;
    const int lh = ui_.lineHeight();

    ui_.box(rc.x, rc.y, rc.w, rc.h, s ? pal::kAccent : pal::kPanel, s ? pal::kLight : pal::kAccent, 4);
    const Color textColor = s ? pal::kBg : pal::kLight;

    // Значок платформы
    const char* badge = r.cgbFlag == 0xC0 ? "GBC" : (r.cgbFlag == 0x80 ? "GB+GBC" : "GB");
    const Color badgeColor = r.cgbFlag ? pal::kGbc : (s ? pal::kBg : pal::kLight);
    const int bw = ui_.textWidth(badge) + 10;
    ui_.fill(rc.x + 8, rc.y + 8, bw, lh + 2, badgeColor);
    ui_.text(rc.x + 13, rc.y + 9, badge, r.cgbFlag ? pal::kBg : (s ? pal::kAccent : pal::kBg));
    if (!r.supported) {
        const std::string warn = "не поддерж.";
        ui_.text(rc.x + rc.w - 10 - ui_.textWidth(warn), rc.y + 9, warn, s ? pal::kBg : pal::kWarn);
    }

    // Название (до двух строк)
    const std::vector<std::string> lines = ui_.wrap(r.title, rc.w - 30, 2);  // справа место под курсор
    for (int k = 0; k < static_cast<int>(lines.size()); ++k) ui_.text(rc.x + 8, rc.y + 18 + lh + k * lh, lines[k], textColor);

    if (s) {  // курсор-треугольник, как в меню старых игр
        const bool blink = (SDL_GetTicks() / 400) % 2 == 0;
        if (blink) ui_.text(rc.x + rc.w - 18, rc.y + rc.h - lh - 6, ">", pal::kBg);
    }
}

void App::drawSettings() {
    const int lh = ui_.lineHeight();
    ui_.text(kGridX + 3, 17, "НАСТРОЙКИ", pal::kShadow, 2);
    ui_.text(kGridX, 14, "НАСТРОЙКИ", pal::kAccent, 2);

    const bool blink = (SDL_GetTicks() / 300) % 2 == 0;
    for (int i = 0; i < kSetCount; ++i) {
        const Rect r = settingsRow(i);
        const bool s = i == setSel_;
        if (s) ui_.box(r.x, r.y, r.w, r.h, pal::kAccent, pal::kLight, 3);
        const Color c = s ? pal::kBg : pal::kLight;
        const int ty = r.y + (r.h - lh) / 2;

        std::string label, value;
        if (i < 8) {
            const int bit = kMenuButtons[i];
            label = std::string("Кнопка ") + kButtonLabels[bit];
            value = (s && waitingKey_) ? (blink ? "[ нажмите клавишу ]" : "") : "[ " + keyName(settings_.keys[bit]) + " ]";
        } else if (i == kSetVolume) {
            label = "Громкость";
            value = fmt("<  %3d%%  >", settings_.volume);
        } else if (i == kSetFolder) {
            label = "Папка с ромами";
            value = ui_.fit(platform::toUtf8(settings_.romDir), r.w - 300 - 12, true);
        } else if (i == kSetResetKeys) {
            label = "Сбросить клавиши";
        } else {
            label = "Назад";
        }
        ui_.text(r.x + 14, ty, (s ? "> " : "  ") + label, c);
        if (!value.empty()) ui_.text(r.x + 300, ty, value, s ? pal::kBg : pal::kAccent);

        if (i == 7 || i == kSetFolder) ui_.fill(r.x, r.y + r.h + 2, r.w, 1, pal::kPanel);  // разделители групп
    }

    // Громкость: полоска
    {
        const Rect r = settingsRow(kSetVolume);
        const int bx = r.x + 300 + ui_.textWidth("<  100%  >") + 16, bw = r.x + r.w - 14 - bx;
        const Color frameC = setSel_ == kSetVolume ? pal::kBg : pal::kAccent;
        if (bw > 20) {
            ui_.frame(bx, r.y + 9, bw, r.h - 18, frameC, 2);
            ui_.fill(bx + 3, r.y + 12, (bw - 6) * settings_.volume / 100, r.h - 24, frameC);
        }
    }

    if (toastActive()) ui_.text(kGridX, 528, ui_.fit(toast_, kW - 2 * kGridX), pal::kWarn);
    ui_.fill(kGridX, 550, kW - 2 * kGridX, 2, pal::kPanel);
    const char* hint = waitingKey_ ? "Нажмите новую клавишу   Esc: отмена"
                                   : (setSel_ == kSetVolume ? "Стрелки влево/вправо: громкость   Esc: назад"
                                                            : "Enter: изменить   Esc: назад");
    ui_.text(kGridX, 554, hint, pal::kDim);
}

void App::drawPause() {
    if (game_) drawGameToCanvas();
    ui_.fill(0, 0, kW, kH, {0, 0, 0, 170});

    const int lh = ui_.lineHeight();
    const int px = 120, py = 140, pw = kW - 240, ph = 92 + kPauseCount * 40 + 8;
    ui_.box(px, py, pw, ph, pal::kBg, pal::kAccent, 6);
    ui_.textCentered(kW / 2, py + 14, "ПАУЗА", pal::kAccent, 2);
    if (game_) ui_.textCentered(kW / 2, py + 14 + lh * 2 + 4, ui_.fit(game_->title, pw - 30), pal::kDim);

    for (int i = 0; i < kPauseCount; ++i) {
        const Rect r = pauseRow(i);
        const bool s = i == pauseSel_;
        if (s) ui_.box(r.x, r.y, r.w, r.h, pal::kAccent, pal::kLight, 3);
        ui_.textCentered(kW / 2, r.y + (r.h - lh) / 2, kPauseItems[i], s ? pal::kBg : pal::kLight);
    }
    ui_.textCentered(kW / 2, py + ph + 20, "Esc: продолжить игру", pal::kLight);
}

void App::saveScreenshot() {
    if (!game_) return;
    fs::path file = shotPath_;
    if (file.empty()) {
        const fs::path dir = platform::exeDir() / "screenshots";
        std::error_code ec;
        fs::create_directories(dir, ec);
        char stamp[32];
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
        file = dir / platform::fromUtf8(platform::toUtf8(game_->path.stem()) + "_" + stamp + ".bmp");
    }
    const auto& frame = game_->bus.ppu().frame();
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint32_t*>(frame.data()), gb::kScreenW, gb::kScreenH,
                                                        32, gb::kScreenW * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    const bool ok = SDL_SaveBMP(s, platform::toUtf8(file).c_str()) == 0;
    SDL_FreeSurface(s);
    if (shotPath_.empty()) toast(ok ? "Снимок сохранён: " + platform::toUtf8(file.filename()) : "Не удалось сохранить снимок");
}

void App::saveCanvasShot() {
    std::vector<uint32_t> px(static_cast<size_t>(kW) * kH);
    if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ARGB8888, px.data(), kW * 4) != 0) return;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), kW, kH, 32, kW * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    SDL_SaveBMP(s, platform::toUtf8(shotPath_).c_str());
    SDL_FreeSurface(s);
}

int main(int argc, char** argv) {
    App app;
    return app.run(argc, argv);
}
