#include "pages/connect-menu/ConnectMenu.h"

#include <windows.h>

#include <algorithm>
#include <map>

#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/gang-mode/GangMode.h"
#include "entities/session/Flow.h"
#include "features/progress/SkinChoice.h"
#include "features/progress/SkinPictures.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
using w::kSaBlue, w::kSaSlate, w::kSaMuted, w::kSaRed, w::kSaLine, w::kSaGreen;
constexpr uint32_t kText = 0xFFE6E9EF;
constexpr uint32_t kGold = 0xFFE2C063;  // the game's mission-title cream

bool g_visible = false;
bool g_autoShown = false;
int g_tab = 0;  // 0 "connect", 1 "create"
std::wstring g_nick, g_addr, g_port, g_password;
std::string g_formError;
int g_slotIndex = 0;  // index into the slot list
std::vector<flow::SaveSlotInfo> g_slots;
uint64_t g_slotsTime = 0;
uint64_t g_copiedMs = 0;

void CopyToClipboard(const std::string& utf8) {
    std::wstring text = Utf8ToWide(utf8);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        else g_copiedMs = NowMs();
    }
    CloseClipboard();
}

// the save slots and, after them, a new game (the host plays its prologue alone, partners join at the bike)
void RefreshSlots() {
    g_slots = flow::ListSaveSlots();
    flow::SaveSlotInfo fresh;
    fresh.slot = -1;
    fresh.exists = true;
    fresh.title = TR("Новая игра", "New game");
    g_slots.push_back(fresh);
    g_slotsTime = NowMs();
}

uint32_t Shade(uint32_t argb, float k) {  // darker (k < 1) or lighter (k > 1)
    auto ch = [&](int sh) { return (uint32_t)std::clamp((float)((argb >> sh) & 0xFF) * k, 0.0f, 255.0f) << sh; };
    return (argb & 0xFF000000) | ch(16) | ch(8) | ch(0);
}

// The character, standing: drawn in its colours from simple shapes (a shadow on the floor, the shoes, the legs in the
// trousers, the top with its sleeves, the arms, the neck and the head with the hair, a cap or a bandana), feet at
// footY, h tall. It breathes and its arms swing a little.
void DrawFigure(float cx, float footY, float h, const skin::Choice& c) {
    auto& g = Gfx();
    float t = (float)(NowMs() % 4000) / 4000.0f * 6.2831853f;
    float breath = std::sin(t) * h * 0.004f, sway = std::sin(t) * h * 0.006f;
    const uint32_t skinTone = 0xFF5C3A26, skinDark = Shade(skinTone, 0.78f);
    auto Y = [&](float k) { return footY - h * k; };
    g.Ellipse(cx, footY + h * 0.005f, h * 0.17f, h * 0.025f, 0x66000000);                   // the shadow
    float hipW = h * 0.055f, legW = h * 0.082f;
    for (int side = -1; side <= 1; side += 2) {
        float hx = cx + side * hipW, ax = cx + side * h * 0.07f;
        g.Line(hx, Y(0.50f), ax, Y(0.05f), legW, side < 0 ? Shade(c.pants, 0.85f) : c.pants);  // a leg
        g.Ellipse(ax + side * h * 0.012f, Y(0.025f), h * 0.055f, h * 0.026f, c.shoes);        // its shoe
    }
    g.Rect(cx - h * 0.105f, Y(0.535f), h * 0.21f, h * 0.04f, Shade(c.pants, 0.7f));  // the belt line
    // the top: shoulders to the waist
    float sh = Y(0.80f) + breath, waist = Y(0.515f);
    uint32_t top = c.shirt, topDark = Shade(c.shirt, 0.82f);
    g.Poly4(cx - h * 0.135f, sh, cx + h * 0.135f, sh, cx + h * 0.105f, waist, cx - h * 0.105f, waist, top);
    g.Poly4(cx + h * 0.02f, sh, cx + h * 0.135f, sh, cx + h * 0.105f, waist, cx + h * 0.02f, waist, topDark);  // its shaded side
    if (c.top == skin::TOP_TANK) {  // the vest's straps leave the shoulders and the chest bare
        g.Tri(cx - h * 0.05f, sh - 1, cx + h * 0.05f, sh - 1, cx, sh + h * 0.07f, skinTone);
        g.Poly4(cx - h * 0.135f, sh, cx - h * 0.085f, sh, cx - h * 0.095f, sh + h * 0.05f, cx - h * 0.13f, sh + h * 0.05f, skinTone);
        g.Poly4(cx + h * 0.085f, sh, cx + h * 0.135f, sh, cx + h * 0.13f, sh + h * 0.05f, cx + h * 0.095f, sh + h * 0.05f, skinDark);
    } else {
        g.Tri(cx - h * 0.035f, sh - 1, cx + h * 0.035f, sh - 1, cx, sh + h * 0.035f, skinTone);  // the collar
    }
    // the arms: shoulder, elbow, hand; the sleeves over them as far as they go
    float armW = h * 0.062f;
    for (int side = -1; side <= 1; side += 2) {
        float s0x = cx + side * h * 0.135f, s0y = sh + h * 0.025f;
        float ex = cx + side * h * 0.165f + side * sway * 0.5f, ey = Y(0.635f) + breath;
        float hx = cx + side * h * 0.16f + side * sway, hy = Y(0.48f);
        uint32_t skinC = side < 0 ? skinTone : skinDark;
        uint32_t upper = c.top == skin::TOP_TANK ? skinC : (side < 0 ? top : topDark);
        uint32_t lower = c.top == skin::TOP_LONG ? upper : skinC;
        g.Line(ex, ey, hx, hy, armW * 0.9f, lower);
        g.Line(s0x, s0y, ex, ey, armW, upper);
        if (c.top == skin::TOP_TEE) g.Line(s0x, s0y, (s0x + ex) * 0.5f, (s0y + ey) * 0.5f, armW * 1.18f, upper);  // the sleeve
        g.Ellipse(hx, hy + h * 0.012f, armW * 0.55f, armW * 0.62f, skinC);  // the hand
    }
    // the neck and the head
    float hy = Y(0.878f) + breath, hr = h * 0.07f;
    g.Rect(cx - h * 0.026f, hy + hr * 0.6f, h * 0.052f, sh - (hy + hr * 0.6f) + 2, skinDark);
    g.Ellipse(cx, hy, hr * 0.92f, hr * 1.08f, skinTone);
    g.Ellipse(cx + hr * 0.35f, hy + hr * 0.05f, hr * 0.5f, hr * 0.95f, skinDark, 20);  // the shaded cheek
    g.Ellipse(cx - hr * 0.88f, hy + hr * 0.1f, hr * 0.16f, hr * 0.24f, skinTone, 12);   // the ears
    g.Ellipse(cx + hr * 0.88f, hy + hr * 0.1f, hr * 0.16f, hr * 0.24f, skinDark, 12);
    if (c.head == skin::HEAD_CAP) {
        g.Ellipse(cx, hy - hr * 0.45f, hr * 0.98f, hr * 0.62f, c.hat);
        g.Rect(cx - hr * 0.98f, hy - hr * 0.45f, hr * 1.96f, hr * 0.28f, c.hat);
        g.Ellipse(cx, hy - hr * 0.2f, hr * 1.12f, hr * 0.2f, Shade(c.hat, 0.72f));  // the peak, to the front
    } else if (c.head == skin::HEAD_BANDANA) {
        g.Ellipse(cx, hy - hr * 0.5f, hr * 0.96f, hr * 0.6f, c.hat);
        g.Rect(cx - hr * 0.95f, hy - hr * 0.5f, hr * 1.9f, hr * 0.3f, c.hat);
        g.Tri(cx + hr * 0.8f, hy - hr * 0.35f, cx + hr * 1.45f, hy - hr * 0.05f, cx + hr * 1.2f, hy + hr * 0.45f, Shade(c.hat, 0.8f));  // the knot
    } else {
        g.Ellipse(cx, hy - hr * 0.62f, hr * 0.9f, hr * 0.48f, c.hat);  // short hair
    }
    float ey = hy - hr * 0.05f;  // the eyes and the brows
    g.Rect(cx - hr * 0.48f, ey, hr * 0.26f, hr * 0.12f, 0xFF140C08);
    g.Rect(cx + hr * 0.18f, ey, hr * 0.26f, hr * 0.12f, 0xFF140C08);
    g.Rect(cx - hr * 0.22f, hy + hr * 0.5f, hr * 0.44f, hr * 0.08f, 0xFF2A1810);  // the mouth
}

// Beside the lobby: the window with the character this player will play, its name, and the arrows under it
// Beside the lobby: every character at once, side by side (the game's own look of the Grove Street ones, CJ drawn),
// the one this player will play lit; a click on one picks it (the arrows under them too)
void CharacterWindow(float x, float y, float W, float H) {
    auto& g = Gfx();
    auto& st = In().State();
    float s = g.Scale();
    int count = 0;
    const skin::Choice* list = skin::Choices(count);
    int index = std::clamp(skin::ChosenIndex(), 0, count - 1);
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, w::kSaLine, std::max(1.0f, s));
    float cx = x + 24 * s, cw = W - 48 * s, cy = y + 26 * s;
    g.Text(FONT_BIG, cx, cy, TR("Персонаж", "Character"), w::kSaBlue, TEXT_SHADOW);
    cy += g.LineHeight(FONT_BIG) + 4 * s;
    g.Text(FONT_SMALL, cx, cy, TR("Нажмите на того, за кого будете играть", "Click the one you will play"), w::kSaMuted, TEXT_SHADOW);
    cy += g.LineHeight(FONT_SMALL) + 12 * s;
    float selH = g.LineHeight(FONT_MENU) + 12 * s;
    float nameH = g.LineHeight(FONT_NORMAL) * 2 + 8 * s;
    float areaH = y + H - 22 * s - selH - nameH - 10 * s - cy;
    // the cards: two rows of two (more: a row more each two)
    int cols = count <= 1 ? 1 : 2, rows = (count + cols - 1) / cols;
    float gap = 10 * s, cardW = (cw - gap * (cols - 1)) / cols, cardH = (areaH - gap * (rows - 1)) / rows;
    static std::map<uint16_t, int> images;  // model -> the renderer's picture
    for (int k = 0; k < count; ++k) {
        const skin::Choice& c = list[k];
        float bx = cx + (k % cols) * (cardW + gap), by = cy + (k / cols) * (cardH + gap);
        bool hover = st.mouseX >= bx && st.mouseX < bx + cardW && st.mouseY >= by && st.mouseY < by + cardH;
        bool lit = k == index;
        // the stage: dark, lighter towards the floor, a glow of the character's colour under its feet
        g.Rect(bx, by, cardW, cardH, 0xFF07090C);
        for (int i = 0; i < 10; ++i) {
            float t = i / 10.0f;
            g.Rect(bx, by + cardH * t, cardW, cardH / 10.0f + 1, WithAlpha(lit ? 0xFF22304A : 0xFF1A2230, t * (hover || lit ? 0.95f : 0.7f)));
        }
        float foot = by + cardH * 0.86f, standH = cardH * 0.80f;
        g.Ellipse(bx + cardW * 0.5f, foot, cardW * 0.34f, cardH * 0.04f, WithAlpha(c.shirt, lit ? 0.35f : 0.15f));
        int pw = 0, ph = 0;
        const uint32_t* px = c.model ? skin::Picture(c.model, pw, ph) : nullptr;
        if (px && pw > 0 && ph > 0) {
            auto it = images.find(c.model);
            if (it == images.end()) it = images.emplace(c.model, g.AddImage(px, pw, ph)).first;
            float h = standH, w = h * (float)pw / (float)ph;
            if (w > cardW * 0.94f) h *= cardW * 0.94f / w, w = cardW * 0.94f;
            g.Image(it->second, bx + cardW * 0.5f - w * 0.5f, foot - h + cardH * 0.01f, w, h);
        } else {
            DrawFigure(bx + cardW * 0.5f, foot, standH * 0.98f, c);
        }
        std::string label = Format("%d", k + 1);
        g.Text(FONT_SMALL, bx + 6 * s, by + 4 * s, label, lit ? w::kSaBlue : w::kSaMuted, TEXT_SHADOW);
        g.RectOutline(bx, by, cardW, cardH, lit ? w::kSaBlue : hover ? 0xFF6A7A8B : w::kSaLine, std::max(1.0f, (lit ? 2.0f : 1.0f) * s));
        if (hover && st.lmbPressed && !lit) skin::SetChoice(k);
    }
    cy += areaH + 10 * s;
    const skin::Choice& c = list[std::clamp(skin::ChosenIndex(), 0, count - 1)];
    float ty = cy;
    for (const std::wstring& line : g.Wrap(FONT_NORMAL, Widen(TR(c.name.ru, c.name.en)), cw)) {
        g.TextW(FONT_NORMAL, cx + cw * 0.5f, ty, line, 0xFFFFFFFF, TEXT_CENTER | TEXT_SHADOW);
        ty += g.LineHeight(FONT_NORMAL);
    }
    if (c.model)
        g.Text(FONT_SMALL, cx + cw * 0.5f, ty, TR("Без магазинов одежды", "No clothes shops"), w::kSaMuted, TEXT_CENTER | TEXT_SHADOW);
    cy += nameH;
    int step = w::Selector("skinsel", cx, cy, cw, selH, Format("%d / %d", index + 1, count));
    if (step) skin::SetChoice((index + step + count) % count);
}

// Beside the lobby in the gang war: the four gangs, each with who plays it; a click on one plays it. A gang nobody plays
// is the computer's or left out: the host switches that on its card. Under them the host's rule of the win.
void GangWindow(float x, float y, float W, float H) {
    using namespace gangmode;
    auto& g = Gfx();
    auto& st = In().State();
    float s = g.Scale();
    bool host = Net().IsHost();
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, w::kSaLine, std::max(1.0f, s));
    float cx = x + 24 * s, cw = W - 48 * s, cy = y + 26 * s;
    g.Text(FONT_BIG, cx, cy, TR("Банда", "Gang"), w::kSaBlue, TEXT_SHADOW);
    cy += g.LineHeight(FONT_BIG) + 4 * s;
    g.Text(FONT_SMALL, cx, cy, TR("Нажмите на банду, за которую будете играть", "Click the gang you will play"), w::kSaMuted, TEXT_SHADOW);
    cy += g.LineHeight(FONT_SMALL) + 12 * s;
    float lh = g.LineHeight(FONT_SMALL), rowH = g.LineHeight(FONT_MENU) + 12 * s;
    float rulesH = host ? rowH * 2 + 20 * s : lh * 2 + 10 * s;
    float areaH = y + H - 22 * s - rulesH - cy;
    float gap = 10 * s, cardW = (cw - gap) / 2, cardH = (areaH - gap) / 2;
    uint8_t mine = MyGang();
    for (int k = 0; k < kGangs; ++k) {
        const GangInfo& gi = Info(k);
        float bx = cx + (k % 2) * (cardW + gap), by = cy + (k / 2) * (cardH + gap);
        bool hover = st.mouseX >= bx && st.mouseX < bx + cardW && st.mouseY >= by && st.mouseY < by + cardH;
        bool lit = k == mine;
        g.Rect(bx, by, cardW, cardH, lit ? WithAlpha(gi.argb, 0.22f) : 0xFF0B0E12);
        g.RectOutline(bx, by, cardW, cardH, lit ? gi.argb : hover ? 0xFF6A7A8B : w::kSaLine, std::max(1.0f, (lit ? 2.0f : 1.0f) * s));
        float ty = by + 8 * s;
        for (const std::wstring& line : g.Wrap(FONT_NORMAL, Widen(Name(k)), cardW - 16 * s)) {
            g.TextW(FONT_NORMAL, bx + 8 * s, ty, line, gi.argb, TEXT_SHADOW);
            ty += g.LineHeight(FONT_NORMAL);
        }
        ty += 4 * s;
        int players = 0;
        auto who = [&](uint8_t id) {
            if (GangOf(id) != k) return;
            std::string n = id == Net().MyId() ? Cfg().nickname : Net().PeerName(id);
            if (Leader(id)) n += TR(" (лидер)", " (leader)");
            g.Text(FONT_SMALL, bx + 8 * s, ty, n, 0xFFE0E0E0, TEXT_SHADOW);
            ty += lh;
            ++players;
        };
        who(Net().MyId());
        for (auto& [id, peer] : Net().Peers()) who(id);
        // the gang's bot: a switch at the card's foot (the host's; the others see how it stands). It plays only a gang
        // nobody picked: with players in it the switch says so
        bool clickedToggle = false;
        {
            bool ai = Rules().ai[k] != 0;
            float bh = lh + 10 * s, bw = cardW - 16 * s, bxx = bx + 8 * s, byy = by + cardH - bh - 8 * s;
            bool over = host && st.mouseX >= bxx && st.mouseX < bxx + bw && st.mouseY >= byy && st.mouseY < byy + bh;
            uint32_t col = players ? w::kSaMuted : ai ? 0xFF4FD06A : 0xFFE05050;
            g.Rect(bxx, byy, bw, bh, over ? WithAlpha(col, 0.30f) : WithAlpha(col, 0.14f));
            g.RectOutline(bxx, byy, bw, bh, over ? 0xFFFFFFFF : col, std::max(1.0f, s));
            std::string label = std::string(TR("Бот: ", "Bot: ")) + (ai ? TR("ВКЛ", "ON") : TR("ВЫКЛ", "OFF"));
            if (players) label += TR(" (играют люди)", " (players in it)");
            else label += ai ? TR(" — играет компьютер", ": the computer plays it") : TR(" — банды нет в игре", ": left out");
            g.Text(FONT_SMALL, bxx + bw * 0.5f, byy + 5 * s, label, players ? w::kSaMuted : 0xFFFFFFFF, TEXT_CENTER | TEXT_SHADOW);
            if (over && st.lmbPressed) {
                Rules().ai[k] = ai ? 0 : 1;
                SendRoster();
                clickedToggle = true;
            }
            if (host && !players && hover && !over)
                g.Text(FONT_SMALL, bxx, byy - lh - 2 * s, TR("Кнопка ниже — вкл/выкл бота", "The button below: the bot on/off"), w::kSaMuted,
                       TEXT_SHADOW);
        }
        if (hover && st.lmbPressed && !clickedToggle && !lit) Pick((uint8_t)k);
    }
    cy += areaH + 10 * s;
    Settings& r = Rules();
    std::string rule = r.rule == WIN_ALL     ? std::string(TR("Победа: все районы", "Win: every territory"))
                       : r.rule == WIN_COUNT ? Format(TR("Победа: %d районов", "Win: %d territories"), (int)r.count)
                                             : Format(TR("Победа: больше всех за %d мин", "Win: the most in %d min"), (int)r.minutes);
    if (!host) {
        g.Text(FONT_SMALL, cx, cy, rule, 0xFFE0E0E0, TEXT_SHADOW);
        g.Text(FONT_SMALL, cx, cy + lh, TR("Правила выбирает хост", "The host sets the rules"), w::kSaMuted, TEXT_SHADOW);
        return;
    }
    if (int d = w::Selector("gwrule", cx, cy, cw, rowH, rule)) {
        r.rule = (uint8_t)((r.rule + d + 3) % 3);
        SendRoster();
    }
    cy += rowH + 8 * s;
    if (r.rule == WIN_COUNT) {
        if (int d = w::Selector("gwcount", cx, cy, cw, rowH, Format(TR("Районов для победы: %d", "Territories to win: %d"), (int)r.count))) {
            r.count = (uint8_t)std::clamp((int)r.count + d * 5, 5, 120);
            SendRoster();
        }
    } else if (r.rule == WIN_TIME) {
        if (int d = w::Selector("gwtime", cx, cy, cw, rowH, Format(TR("Время: %d мин", "Time: %d min"), (int)r.minutes))) {
            r.minutes = (uint8_t)std::clamp((int)r.minutes + d * 5, 10, 180);
            SendRoster();
        }
    }
}

void LoadFields() {
    auto& cfg = Cfg();
    g_nick = Widen(cfg.nickname);
    g_addr = Widen(cfg.host);
    g_port = std::to_wstring(cfg.port);
    g_password = Widen(cfg.password);
    g_tab = cfg.wantsHost ? 1 : 0;
    g_formError.clear();
    RefreshSlots();
    // preselect the first save; without any, the new game
    g_slotIndex = 0;
    for (size_t i = 0; i < g_slots.size(); ++i)
        if (g_slots[i].exists) {
            g_slotIndex = (int)i;
            break;
        }
}

bool ApplyNick() {
    std::string nick = SanitizeNickname(Narrow(g_nick));
    if (nick.size() < 2) {
        g_formError = TR("В нике должно быть от 2 до 20 символов (буквы, цифры, знаки _-.[]).",
                         "A nickname needs 2 to 20 characters (letters, digits, _-.[]).");
        w::SetFocus("nick");
        return false;
    }
    g_formError.clear();
    Cfg().nickname = nick;
    g_nick = Widen(nick);
    return true;
}

uint16_t PortValue() {
    int p = _wtoi(g_port.c_str());
    if (p <= 0 || p > 65535) p = 7788;
    return (uint16_t)p;
}
}  // namespace

void SetConnectMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    if (visible) {
        LoadFields();
        if (TheChat().IsOpen()) TheChat().Close();
        In().SetKeyboardCapture(true);
        In().SetMouseCapture(true);
    } else {
        In().SetKeyboardCapture(false);
        In().SetMouseCapture(false);
    }
}

bool IsConnectMenuVisible() { return g_visible; }

void ProcessConnectMenu() {
    // Show the window once on the first main-menu frame so the player sees how to start co-op.
    if (!g_autoShown && game::GameState() == game::GAME_STATE_FRONTEND_IDLE && !Cfg().autoConnect) {
        g_autoShown = true;
        SetConnectMenuVisible(true);
    }
    if (g_visible && NowMs() - g_slotsTime > 3000) RefreshSlots();
}

void RenderConnectMenu() {
    if (!g_visible) return;
    auto& g = Gfx();
    float s = g.Scale();
    auto& net = Net();
    bool online = net.Online();
    auto phase = flow::GetPhase();
    bool busy = phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting ||
                phase == flow::Phase::WaitingWorld || phase == flow::Phase::LoadingWorld;
    bool inGame = core::IsPlayerReady();

    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU),
          ch = g.LineHeight(FONT_MENU_SMALL), bh = g.LineHeight(FONT_BIG);
    float rowH = lh + 14 * s;
    float W = 820 * s, H = 690 * s;
    float x = (g.Width() - W) * 0.5f, y = (g.Height() - H) * 0.5f;
    // everyone in the lobby (the host too, for now: to try the characters): the character window right of the lobby
    // (the lobby stays in the middle while both fit)
    bool picker = online && flow::InLobby();
    float pickW = 380 * s, pickGap = 24 * s, pickH = H;
    if (picker) {
        float need = W + pickGap + pickW, margin = 16 * s;
        if (x + need > g.Width() - margin) x = std::max(margin, (g.Width() - need) * 0.5f);
    }
    float cx = x + 52 * s, cw = W - 104 * s;
    float room = mh * 1.6f;  // space for the pointer left of a menu item

    float fy = y + H - 20 * s - sh;       // key legend
    float by = fy - 14 * s - rowH;        // footer items
    float bodyBottom = by - 12 * s;

    g.Rect(0, 0, g.Width(), g.Height(), 0x99000000);
    w::BeginFrame();
    float cy = w::Panel(x, y, W, H, "SA:COOP") + 8 * s;
    float vx = x + W - 52 * s;
    g.Text(FONT_MENU_SMALL, vx, y + 36 * s, "v" COOP_VERSION_STR, kSaSlate, TEXT_RIGHT);
    // the language switch left of the version, the pointer at the active one; the choice is saved at once
    float lx = vx - g.TextWidth(FONT_MENU_SMALL, "v" COOP_VERSION_STR) - 34 * s;
    for (int en = 1; en >= 0; --en) {
        const char* label = en ? "EN" : "RU";
        float tw = g.TextWidth(FONT_MENU_SMALL, label);
        lx -= tw;
        if (w::Tab(en ? "lang1" : "lang0", lx - 6 * s, y + 32 * s, tw + 12 * s, ch + 8 * s, label, Cfg().english == (en != 0), true)) {
            Cfg().english = en != 0;
            flow::SaveConnectSettings();
            RefreshSlots();
        }
        lx -= 52 * s;  // room for the pointer of EN
    }

    auto caption = [&](float px, const std::string& text) { g.Text(FONT_MENU_SMALL, px, cy, text, kSaMuted); };
    // wrapped paragraph, advances cy
    auto para = [&](const std::string& text, uint32_t color, FontId f = FONT_SMALL, bool centre = false) {
        for (const std::wstring& line : g.Wrap(f, Widen(text), cw)) {
            g.TextW(f, centre ? cx + cw * 0.5f : cx, cy, line, color, TEXT_SHADOW | (centre ? TEXT_CENTER : 0));
            cy += g.LineHeight(f);
        }
    };
    // a menu item whose text starts at px (the pointer hangs to the left of it)
    auto item = [&](const char* id, float px, float py, const std::string& label, bool enabled = true, bool lit = false) {
        return w::Button(id, px - room, py, g.TextWidth(FONT_MENU, label) + 2 * room, rowH, label, enabled, lit);
    };

    if (online) {
        // ---- in a session: who we are, the address for friends, the players
        bool host = net.IsHost();
        bool lobby = flow::InLobby();
        g.Text(FONT_BIG, cx, cy,
               lobby ? TR("Лобби", "Lobby") : host ? TR("Сервер запущен", "Server running") : TR("Вы в игре", "Connected"), kSaBlue,
               TEXT_SHADOW);
        cy += bh + 2 * s;
        if (!host) para(Format(TR("Сервер «%s»", "Server \"%s\""), net.ServerName().c_str()), kSaMuted, FONT_NORMAL);
        cy += 22 * s;
        if (host) {
            const Session::Address& a = net.ServerAddress();
            caption(cx, TR("Адрес для друзей", "Address for your friends"));
            cy += ch + 6 * s;
            if (!a.done && a.external.empty()) {
                g.Text(FONT_NORMAL, cx, cy, TR("Узнаём внешний IP...", "Finding out the external IP..."), kSaMuted, TEXT_SHADOW);
                cy += bh;
            } else {
                std::string shown = !a.external.empty() ? a.external : a.lan;
                // our wide capitals, not the game's pricedown: translations swap letters in that font
                g.Text(FONT_BIG, cx, cy, shown.empty() ? TR("не определён", "unknown") : shown, kGold, TEXT_OUTLINE);
                if (!shown.empty()) {
                    bool copied = NowMs() - g_copiedMs < 1500;
                    const std::string label = copied ? TR("Скопировано", "Copied") : TR("Скопировать", "Copy");
                    if (item("copy", cx + cw - g.TextWidth(FONT_MENU, label), cy + (bh - rowH) * 0.5f, label, true, copied))
                        CopyToClipboard(shown);
                }
                cy += bh + 4 * s;
                if (!a.external.empty() && !a.lan.empty()) para(Format(TR("В локальной сети: %s", "In your local network: %s"), a.lan.c_str()), kSaMuted);
                if (a.cgnat)
                    para(TR("Роутер за NAT провайдера (CGNAT): из интернета к вам не подключиться. Нужен белый IP у провайдера "
                            "или VPN (Radmin VPN, ZeroTier); в локальной сети всё работает.",
                            "Your router is behind the provider's NAT (CGNAT): nobody can connect from the internet. You need a "
                            "public IP from the provider or a VPN (Radmin VPN, ZeroTier); the local network works."),
                         kSaRed);
                else if (a.done && a.external.empty())
                    para(TR("Внешний IP не определён: нет доступа в интернет. Подключиться можно только из локальной сети.",
                            "The external IP is unknown: no internet access. Only players in your local network can connect."),
                         kSaRed);
                else if (a.done && !a.upnp)
                    para(Format(TR("UPnP не сработал (%s): откройте UDP-порт %u на роутере вручную, иначе друзья из интернета "
                                   "не подключатся.",
                                   "UPnP did not work (%s): open UDP port %u on your router by hand, or friends from the internet "
                                   "cannot connect."),
                                a.upnpError.empty() ? "?" : a.upnpError.c_str(), (unsigned)Cfg().port),
                         kSaRed);
                else if (a.done)
                    para(Format(TR("Порт %u открыт на роутере через UPnP.", "Port %u is open on your router through UPnP."), (unsigned)Cfg().port),
                         kSaMuted);
            }
            cy += 16 * s;
        }
        int rows = 1 + (int)net.Peers().size();
        caption(cx, Format(TR("Игроки: %d из %d", "Players: %d of %d"), rows, net.MaxPlayers()));
        cy += ch + 8 * s;
        // up to 16: two columns of smaller rows once one column of big ones would not fit
        bool compact = rows > 5;
        int columns = compact ? 2 : 1;
        float gap = 10 * s, colW = (cw - gap * (columns - 1)) / columns;
        float rh = compact ? std::min(sh + 10 * s, (bodyBottom - cy) / ((rows + 1) / 2) - 4 * s) : lh + 14 * s;
        FontId font = compact ? FONT_SMALL : FONT_NORMAL;
        float top = cy;
        int index = 0;
        auto row = [&](uint8_t id, const std::string& name, bool isHost, uint32_t ping, bool me) {
            int col = index % columns, line = index / columns;
            ++index;
            float rx = cx + col * (colW + gap), ry = top + line * (rh + 4 * s);
            g.Rect(rx, ry, colW, rh, 0xFF090C10);
            char dot[24];
            snprintf(dot, sizeof dot, "{%06X}●{FFFFFF} ", (unsigned)(PlayerColor(id) & 0xFFFFFF));
            float fh = g.LineHeight(font);
            g.Text(font, rx + 10 * s, ry + (rh - fh) * 0.5f, dot + (me ? name + TR(" (вы)", " (you)") : name),
                   me ? 0xFFFFFFFF : kText, TEXT_SHADOW);
            if (isHost) g.Text(FONT_MENU_SMALL, rx + colW * (compact ? 0.62f : 0.55f), ry + (rh - ch) * 0.5f, TR("хост", "host"), kSaBlue);
            else if (lobby && (me ? flow::LobbyMyReady() : flow::LobbyReady(id)))
                g.Text(FONT_MENU_SMALL, rx + colW * (compact ? 0.62f : 0.55f), ry + (rh - ch) * 0.5f, TR("готов", "ready"), kSaGreen);
            g.Text(FONT_SMALL, rx + colW - 10 * s, ry + (rh - sh) * 0.5f, Format(TR("%u мс", "%u ms"), ping), kSaMuted, TEXT_RIGHT);
            cy = std::max(cy, ry + rh + 4 * s);
        };
        row(net.MyId(), Cfg().nickname, host, net.Rtt(), true);
        for (auto& [id, p] : net.Peers()) row(id, p.name, p.isHost, p.ping, false);
        if (host && net.Peers().empty()) para(TR("Ждём друзей...", "Waiting for your friends..."), kSaMuted);
        if (lobby) {  // the host starts the game for everyone, a partner says it is ready
            cy += 10 * s;
            int ready = 0;
            for (auto& [id, p] : net.Peers())
                if (!p.isHost && flow::LobbyReady(id)) ++ready;
            if (host) {
                if (w::Button("start", cx, cy, cw, bh + 10 * s, TR("Начать игру", "Start the game"), true, true, true))
                    flow::LobbyStart();
                cy += bh + 16 * s;
                para(Format(TR("Готовы: %d из %d. Кто подключится позже, сразу войдёт в игру.",
                               "Ready: %d of %d. Whoever joins later goes straight into the game."),
                            ready, (int)net.Peers().size()),
                     kSaMuted, FONT_SMALL, true);
            } else {
                bool me = flow::LobbyMyReady();
                if (w::Button("ready", cx, cy, cw, bh + 10 * s, me ? TR("Не готов", "Not ready") : TR("Готов", "Ready"), true, !me, true))
                    flow::LobbySetReady(!me);
                cy += bh + 16 * s;
                para(TR("Игра начнётся, когда хост нажмёт «Начать игру».",
                        "The game starts when the host presses \"Start the game\"."),
                     kSaMuted, FONT_SMALL, true);
            }
        }
    } else if (busy) {
        // ---- connecting / starting / loading, in the middle of the body
        float barH = 6 * s;
        cy += std::max(0.0f, (bodyBottom - cy - (mh * 2 + 18 * s + barH + 16 * s + sh)) * 0.5f);
        para(flow::StatusText(), kSaBlue, FONT_MENU, true);
        cy += 18 * s;
        g.Rect(cx, cy, cw, barH, 0xFF141A21);
        float p = phase == flow::Phase::WaitingWorld ? net.WorldProgress() : -1.0f;
        if (p >= 0) {
            g.Rect(cx, cy, cw * std::clamp(p, 0.0f, 1.0f), barH, kSaBlue);
        } else {  // nothing to measure: a block sliding back and forth
            float t = (float)(NowMs() % 1600) / 800.0f;
            float bw = cw * 0.25f;
            g.Rect(cx + (cw - bw) * (t < 1 ? t : 2 - t), cy, bw, barH, kSaBlue);
        }
        cy += barH + 16 * s;
        if (phase == flow::Phase::WaitingWorld && p < 0)
            para(TR("Хост сохраняет мир. Скоро он загрузится у вас.", "The host is saving the world. It will load for you soon."),
                 kSaMuted, FONT_SMALL, true);
    } else {
        // ---- the form: the tabs are menu items, the active one carries the pointer
        float tx = cx;
        for (int t = 0; t < 2; ++t) {
            const char* label = t ? TR("Создать игру", "Host a game") : TR("Подключиться", "Join a game");
            float tw = g.TextWidth(FONT_MENU, label);
            if (w::Tab(t ? "tab1" : "tab0", tx - room, cy, tw + 2 * room, rowH, label, g_tab == t)) {
                g_tab = t;
                g_formError.clear();
            }
            tx += tw + room * 2.2f;
        }
        cy += rowH + 4 * s;
        g.Rect(cx, cy, cw, std::max(1.0f, s), kSaLine);
        cy += 16 * s;

        caption(cx, TR("Ник", "Nickname"));
        cy += ch + 6 * s;
        bool enter = w::TextBox("nick", cx, cy, 320 * s, rowH, g_nick, 20);
        cy += rowH + 14 * s;

        int hostSlot = 0;
        if (g_tab == 0) {
            caption(cx, TR("IP-адрес хоста", "Host's IP address"));
            cy += ch + 6 * s;
            const char* paste = TR("Вставить", "Paste");
            float pw = g.TextWidth(FONT_MENU, paste) + room * 1.9f;
            enter |= w::TextBox("addr", cx, cy, cw - pw, rowH, g_addr, 64, false, false,
                                TR("например, 85.12.34.56 или 85.12.34.56:7788", "for example, 85.12.34.56 or 85.12.34.56:7788"));
            if (item("paste", cx + cw - g.TextWidth(FONT_MENU, paste), cy, paste)) {
                std::wstring clip = Widen(Trim(Narrow(w::ClipboardText())));
                if (!clip.empty()) g_addr = clip.substr(0, 64);
            }
        } else {
            // the mode (the story, or the gang war: a new game for everyone) and, for the story, the save
            float mw = inGame ? 0.0f : cw * 0.46f, gap = inGame ? 0.0f : 14 * s;
            if (!inGame) caption(cx, TR("Режим", "Mode"));
            caption(cx + mw + gap, TR("Сохранение", "Save"));
            cy += ch + 6 * s;
            int& mode = Cfg().gameMode;
            if (!inGame) {
                int d = w::Selector("mode", cx, cy, mw, rowH, mode == 1 ? TR("Война банд: ЛС", "Gang war: LS") : TR("Сюжет", "The story"));
                if (d) mode = 1 - mode;
            }
            float sx = cx + mw + gap, sw = cw - mw - gap;
            if (inGame || mode == 1) {
                g.Rect(sx, cy, sw, rowH, 0xFF090C10);
                g.Text(FONT_NORMAL, sx + 12 * s, cy + (rowH - lh) * 0.5f,
                       inGame ? TR("Текущая игра", "Current game") : TR("Новая игра для всех", "A new game for all"), kText, TEXT_SHADOW);
                if (!inGame) hostSlot = -1;
            } else {
                int d = w::Selector("slot", sx, cy, sw, rowH, g_slots[g_slotIndex].title);
                if (d) g_slotIndex = (g_slotIndex + d + (int)g_slots.size()) % (int)g_slots.size();
                if (g_slots[g_slotIndex].exists) hostSlot = g_slots[g_slotIndex].slot;
            }
            if (inGame) mode = 0;  // hosted from inside a game: the story goes on
        }
        cy += rowH + 14 * s;

        caption(cx, TR("Порт", "Port"));
        caption(cx + 170 * s, TR("Пароль", "Password"));
        cy += ch + 6 * s;
        enter |= w::TextBox("port", cx, cy, 150 * s, rowH, g_port, 5, false, true);
        enter |= w::TextBox("pass", cx + 170 * s, cy, cw - 170 * s, rowH, g_password, 32, true, false, TR("необязательно", "optional"));
        cy += rowH + 22 * s;
        if (g_tab == 1) {  // the host decides for the whole game; saved with the other settings when the server starts
            w::Checkbox("ff", cx, cy, TR("Урон по напарнику", "Friendly fire"), Cfg().friendlyFire);
            w::Checkbox("nocheat", cx + 270 * s, cy, TR("Без читов", "No cheats"), Cfg().noCheats);  // for all, the host too
            // the places on the server, the host's own among them
            int& places = Cfg().maxPlayers;
            float sw = 250 * s;
            int d = w::Selector("places", cx + cw - sw, cy - (rowH - lh * 0.8f) * 0.5f, sw, rowH,
                                Format(TR("Игроков: до %d", "Players: up to %d"), places));
            if (d) places = (places - 2 + d + 15) % 15 + 2;  // 2..16 round
            cy += lh * 0.8f + 22 * s;
        }

        // the main action: a big menu item in the middle, lit like the selected entry of the game's menus
        bool can = g_tab == 0 ? !Trim(Narrow(g_addr)).empty() : inGame || hostSlot != 0;
        if ((w::Button("go", cx, cy, cw, bh + 10 * s, g_tab == 0 ? TR("Подключиться", "Connect") : TR("Создать сервер", "Start the server"),
                       can, true, true) ||
             (enter && can)) &&
            ApplyNick()) {
            if (g_tab == 0) flow::Join(Trim(Narrow(g_addr)), PortValue(), Narrow(g_password));
            else flow::Host(inGame ? 0 : hostSlot, PortValue(), Narrow(g_password));
        }
        cy += bh + 10 * s + 14 * s;

        std::string error = !g_formError.empty() ? g_formError : flow::LastError();
        if (!error.empty()) para(error, kSaRed, FONT_NORMAL, true);
        else if (g_tab == 0 && inGame)
            para(TR("Текущая игра закроется. Вместо неё загрузится мир друга.",
                    "Your current game will close, and your friend's world will load instead."),
                 kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && !hostSlot)
            para(TR("В этом слоте нет сохранения.", "This slot has no save."), kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && Cfg().gameMode == 1)
            para(TR("Все начнут новую игру без сюжета и пролога. Банду каждый выберет в лобби.",
                    "Everyone starts a new game with no story and no prologue. Each picks a gang in the lobby."),
                 kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && hostSlot < 0)
            para(TR("Пролог вы пройдёте сами, а друг появится рядом, когда CJ сядет на велосипед.",
                    "You play the prologue alone, and your friend appears next to you when CJ gets on the bike."),
                 kSaMuted, FONT_SMALL, true);
    }

    // ---- footer: like "Back" on the game's pages
    g.Rect(cx, bodyBottom, cw, std::max(1.0f, s), kSaLine);
    if (online || busy) {
        if (item("leave", cx, by, online ? TR("Отключиться", "Disconnect") : TR("Отмена", "Cancel"))) flow::Leave();
    }
    std::string close = online || busy || inGame ? TR("Закрыть", "Close") : TR("Одиночная игра", "Single player");
    if (item("close", cx + cw - g.TextWidth(FONT_MENU, close), by, close)) SetConnectMenuVisible(false);
    g.Text(FONT_SMALL, cx, fy,
           TR("{ACCBF1}F9{8D9CAD}  окно     {ACCBF1}T{8D9CAD}  чат     {ACCBF1}B{8D9CAD}  голос     "
              "{ACCBF1}G{8D9CAD}  сесть пассажиром     {ACCBF1}F2{8D9CAD}  игроки",
              "{ACCBF1}F9{8D9CAD}  window     {ACCBF1}T{8D9CAD}  chat     {ACCBF1}B{8D9CAD}  voice     "
              "{ACCBF1}G{8D9CAD}  passenger     {ACCBF1}F2{8D9CAD}  players"),
           kSaMuted, TEXT_SHADOW);
    if (picker && gangmode::On()) GangWindow(x + W + pickGap, y, pickW, pickH);
    else if (picker) CharacterWindow(x + W + pickGap, y, pickW, pickH);
    w::EndFrame();
    w::DrawCursor();
    // Esc closes even when a text box is focused (handled in OnKeyDown)
}

}  // namespace coop::ui
