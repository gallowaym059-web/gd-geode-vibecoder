// CBT - Click Before Tick  (v1.1)
// Input offset, auto clicker, timescale, TPS sub-stepping, anti-lag,
// music resync, auto restart, anti-kick, stats HUD, draggable menu button.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Config + runtime state
// ---------------------------------------------------------------------------
namespace cbt {

    struct Config {
        // Input
        bool offsetOn = false;     double offsetMs = 0.0;
        bool clickSound = false;
        // Clicker
        bool clickerOn = false;    double cps = 10.0;
        bool clickerHold = true;
        // Speed
        bool timescaleOn = false;  double timescale = 1.0;
        bool tpsOn = false;        double tps = 480.0;
        bool fpsOn = false;        double fps = 120.0;
        // Smooth
        bool antiLag = false;      double lagSens = 2.5;
        bool musicSync = false;    double syncMs = 80.0;
        // Game
        bool autoRestart = false;  double restartDelay = 0.0;
        bool antiKick = false;
        bool safeMode = true;
        // Misc
        bool showStats = true;
        bool hideBtnInLevel = false;
        double btnOpacity = 0.7;
        double btnScale = 1.0;
        double btnX = 40.0;
        double btnY = 160.0;
    };

    inline Config cfg;

#define CBT_BOOLS(X) \
    X(offsetOn) X(clickSound) X(clickerOn) X(clickerHold) X(timescaleOn) \
    X(tpsOn) X(fpsOn) X(antiLag) X(musicSync) X(autoRestart) X(antiKick) \
    X(safeMode) X(showStats) X(hideBtnInLevel)

#define CBT_DOUBLES(X) \
    X(offsetMs) X(cps) X(timescale) X(tps) X(fps) X(lagSens) X(syncMs) \
    X(restartDelay) X(btnOpacity) X(btnScale) X(btnX) X(btnY)

    inline void sanitize() {
        cfg.offsetMs = std::clamp(cfg.offsetMs, -350.0, 350.0);
        cfg.cps = std::clamp(cfg.cps, 1.0, 240.0);
        cfg.timescale = std::clamp(cfg.timescale, 0.1, 10.0);
        cfg.tps = std::clamp(cfg.tps, 240.0, 1000.0);
        cfg.fps = std::clamp(cfg.fps, 30.0, 360.0);
        cfg.lagSens = std::clamp(cfg.lagSens, 1.5, 8.0);
        cfg.syncMs = std::clamp(cfg.syncMs, 20.0, 500.0);
        cfg.restartDelay = std::clamp(cfg.restartDelay, 0.0, 3.0);
        cfg.btnOpacity = std::clamp(cfg.btnOpacity, 0.15, 1.0);
        cfg.btnScale = std::clamp(cfg.btnScale, 0.5, 2.0);
    }

    inline void loadCfg() {
        auto m = Mod::get();
#define X(n) cfg.n = m->getSavedValue<bool>(#n, cfg.n);
        CBT_BOOLS(X)
#undef X
#define X(n) cfg.n = m->getSavedValue<double>(#n, cfg.n);
        CBT_DOUBLES(X)
#undef X
        sanitize();
    }

    inline void saveCfg() {
        auto m = Mod::get();
#define X(n) m->setSavedValue<bool>(#n, cfg.n);
        CBT_BOOLS(X)
#undef X
#define X(n) m->setSavedValue<double>(#n, cfg.n);
        CBT_DOUBLES(X)
#undef X
    }

    inline double nowSec() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    struct Pending {
        bool down;
        int button;
        bool p1;
        double due;   // real time (seconds) at which to deliver
    };

    inline std::deque<Pending> pending;
    inline bool delivering = false;     // true while WE call handleButton
    inline bool userHold = false;       // physical jump-button state
    inline bool synthDown = false;      // auto clicker's current state
    inline double acTimer = 0.0;
    inline bool acFresh = true;
    inline double accumulator = 0.0;
    inline std::deque<double> clickTimes;
    inline int attemptClicks = 0;
    inline double frameAvg = 1.0 / 60.0;
    inline double lagDebt = 0.0;
    inline double lastFrameReal = 0.0;
    inline double fpsEma = 60.0;
    inline double lastSyncCheck = 0.0;
    inline bool baselineSet = false;
    inline double musicBaseline = 0.0;
    inline float appliedScale = 1.f;
    inline double exitAllowedUntil = 0.0;
    inline double lastHudUpdate = 0.0;
    inline bool panelOpen = false;
    inline double origInterval = 0.0;
    inline bool fpsApplied = false;

    void openPanel();   // defined after CBTPanel

    inline bool cheating() {
        return (cfg.offsetOn && std::fabs(cfg.offsetMs) > 0.01) ||
               cfg.clickerOn ||
               (cfg.timescaleOn && std::fabs(cfg.timescale - 1.0) > 0.001) ||
               (cfg.tpsOn && cfg.tps > 240.0);
    }

    inline bool inLevel(GJBaseGameLayer* layer) {
        auto pl = PlayLayer::get();
        return pl && layer == static_cast<GJBaseGameLayer*>(pl);
    }

    inline bool live() {
        auto pl = PlayLayer::get();
        return pl && pl->m_started && !pl->m_playerDied;
    }

    inline void registerClick() {
        double t = nowSec();
        clickTimes.push_back(t);
        ++attemptClicks;
        while (!clickTimes.empty() && t - clickTimes.front() > 1.0)
            clickTimes.pop_front();
    }

    inline int currentCPS() {
        double t = nowSec();
        while (!clickTimes.empty() && t - clickTimes.front() > 1.0)
            clickTimes.pop_front();
        return static_cast<int>(clickTimes.size());
    }

    // Calls the game's handleButton without our own hook interfering.
    inline void deliver(GJBaseGameLayer* l, bool down, int button, bool p1) {
        delivering = true;
        l->handleButton(down, button, p1);
        delivering = false;
    }

    inline void flushPending(GJBaseGameLayer* l) {
        while (!pending.empty()) {
            auto p = pending.front();
            pending.pop_front();
            deliver(l, p.down, p.button, p.p1);
        }
    }

    inline void releaseSynth(GJBaseGameLayer* l) {
        if (synthDown) {
            synthDown = false;
            deliver(l, false, 1, true);
        }
        acTimer = 0.0;
        acFresh = true;
    }

    // Auto clicker: one call per physics sub-step.
    inline void clickerStep(GJBaseGameLayer* l, double step) {
        bool want = cfg.clickerOn && (!cfg.clickerHold || userHold);
        if (!want) {
            releaseSynth(l);
            return;
        }

        double rate = std::min(cfg.cps, 0.5 / step);
        double half = 0.5 / rate;
        acTimer += step;

        if (acFresh) {
            acFresh = false;
            acTimer = 0.0;
            synthDown = true;
            registerClick();
            deliver(l, true, 1, true);
            return;
        }

        if (acTimer + 1e-9 >= half) {
            acTimer -= half;
            synthDown = !synthDown;
            if (synthDown) registerClick();
            deliver(l, synthDown, 1, true);
        }
    }

    // Anti-lag: clamp huge frame times and pay the time back gradually.
    inline float lagAdjust(float dt) {
        double d = dt;
        if (d <= 0.0) return dt;

        if (!cfg.antiLag) {
            lagDebt = 0.0;
            if (d < 0.1) frameAvg = frameAvg * 0.95 + d * 0.05;
            return dt;
        }

        double limit = std::max(frameAvg * cfg.lagSens, frameAvg + 0.006);
        if (d > limit) {
            double allowed = frameAvg * 1.25;
            lagDebt = std::min(lagDebt + (d - allowed), 0.4);
            d = allowed;
        } else {
            frameAvg = std::clamp(frameAvg * 0.9 + d * 0.1, 1.0 / 360.0, 1.0 / 20.0);
            if (lagDebt > 0.0) {
                double pay = std::min(lagDebt, d * 0.2);
                d += pay;
                lagDebt -= pay;
            }
        }
        return static_cast<float>(d);
    }

    // Music resync: learn the normal music/level offset, then correct drift.
    inline void musicTick(PlayLayer* pl, double now) {
        if (!cfg.musicSync) return;
        if (now - lastSyncCheck < 0.35) return;
        lastSyncCheck = now;

        if (cfg.timescaleOn || std::fabs(pl->m_gameState.m_timeWarp - 1.f) > 0.001f) {
            baselineSet = false;
            return;
        }

        auto fmod = FMODAudioEngine::sharedEngine();
        if (!fmod || !fmod->isMusicPlaying(0)) return;

        double levelMs = pl->m_gameState.m_levelTime * 1000.0;
        double musicMs = static_cast<double>(fmod->getMusicTimeMS(0));

        if (!baselineSet) {
            if (levelMs > 400.0) {
                musicBaseline = musicMs - levelMs;
                baselineSet = true;
            }
            return;
        }

        double drift = (musicMs - levelMs) - musicBaseline;
        if (std::fabs(drift) > cfg.syncMs) {
            double target = std::max(0.0, levelMs + musicBaseline);
            fmod->setMusicTimeMS(static_cast<unsigned int>(target), true, 0);
        }
    }

    inline void applyTimescale(PlayLayer* pl) {
        float want = cfg.timescaleOn
            ? static_cast<float>(std::clamp(cfg.timescale, 0.1, 10.0))
            : 1.f;
        if (std::fabs(want - appliedScale) < 1e-4f) return;
        pl->updateTimeWarp(want);
        appliedScale = want;
    }

    inline void hudTick(PlayLayer* pl, double now) {
        auto hud = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("cbt-hud"_spr));
        if (!hud) return;
        hud->setVisible(cfg.showStats);
        if (!cfg.showStats) return;
        if (now - lastHudUpdate < 0.2) return;
        lastHudUpdate = now;

        int tpsNow = static_cast<int>(cfg.tpsOn ? cfg.tps : 240.0);
        hud->setString(
            fmt::format(
                "FPS {}  CPS {}  TPS {}  Clicks {}",
                static_cast<int>(std::round(fpsEma)),
                currentCPS(),
                tpsNow,
                attemptClicks
            ).c_str()
        );
    }

    inline void resetRuntime() {
        pending.clear();
        synthDown = false;
        acTimer = 0.0;
        acFresh = true;
        accumulator = 0.0;
        lagDebt = 0.0;
        baselineSet = false;
        attemptClicks = 0;
    }

    inline void applyFps() {
        auto dir = CCDirector::sharedDirector();
        if (!dir) return;
        if (cfg.fpsOn) {
            if (origInterval <= 0.0) origInterval = dir->getAnimationInterval();
            dir->setAnimationInterval(1.0 / cfg.fps);
            fpsApplied = true;
        } else if (fpsApplied && origInterval > 0.0) {
            dir->setAnimationInterval(origInterval);
            fpsApplied = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Draggable circle button (hold 1 second to open the menu, dragging only moves)
// ---------------------------------------------------------------------------
class CBTButton : public CCNode, public CCTouchDelegate {
protected:
    CCSprite* m_spr = nullptr;
    CCLabelBMFont* m_lbl = nullptr;
    bool m_down = false;
    bool m_dragging = false;
    bool m_opened = false;
    bool m_registered = false;
    CCPoint m_startTouch = {0.f, 0.f};
    CCPoint m_startPos = {0.f, 0.f};
    double m_downAt = 0.0;

    float radius() {
        return m_spr->getContentSize().width * 0.5f * this->getScale() * 1.15f;
    }

    void registerTouch() {
        if (m_registered) return;
        m_registered = true;
        CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -600, true);
    }

    void unregisterTouch() {
        if (!m_registered) return;
        m_registered = false;
        CCDirector::sharedDirector()->getTouchDispatcher()->removeDelegate(this);
    }

    void finishTouch() {
        if (m_dragging) {
            cbt::cfg.btnX = this->getPositionX();
            cbt::cfg.btnY = this->getPositionY();
            cbt::saveCfg();
        }
        m_down = false;
        m_dragging = false;
        m_opened = false;
        this->setScale(static_cast<float>(cbt::cfg.btnScale));
    }

public:
    static CBTButton* create() {
        auto ret = new CBTButton();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        m_spr = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
        if (!m_spr) return false;
        this->addChild(m_spr);

        m_lbl = CCLabelBMFont::create("CBT", "bigFont.fnt");
        m_lbl->setScale(0.4f);
        this->addChild(m_lbl);

        this->setID("cbt-button"_spr);

        auto ws = CCDirector::sharedDirector()->getWinSize();
        float x = std::clamp(static_cast<float>(cbt::cfg.btnX), 20.f, ws.width - 20.f);
        float y = std::clamp(static_cast<float>(cbt::cfg.btnY), 20.f, ws.height - 20.f);
        this->setPosition({x, y});

        this->refreshLook();
        this->scheduleUpdate();
        return true;
    }

    void refreshLook() {
        GLubyte op = static_cast<GLubyte>(std::clamp(cbt::cfg.btnOpacity, 0.0, 1.0) * 255.0);
        m_spr->setOpacity(op);
        m_lbl->setOpacity(op);
        m_spr->setColor(cbt::cheating() ? ccc3(255, 130, 130) : ccc3(255, 255, 255));
        if (!m_down) this->setScale(static_cast<float>(cbt::cfg.btnScale));
    }

    void onEnter() override {
        CCNode::onEnter();
        this->registerTouch();
    }

    void onExit() override {
        this->unregisterTouch();
        CCNode::onExit();
    }

    void update(float) override {
        if (!m_registered) this->registerTouch();

        bool hide = false;
        if (cbt::cfg.hideBtnInLevel) {
            if (auto pl = PlayLayer::get()) hide = !pl->m_isPaused;
        }
        this->setVisible(!hide || m_down);

        // Real-time hold detection (not affected by the timescale feature).
        if (m_down && !m_dragging && !m_opened) {
            double held = cbt::nowSec() - m_downAt;
            float prog = static_cast<float>(std::min(held, 1.0));
            this->setScale(static_cast<float>(cbt::cfg.btnScale) * (1.f + 0.2f * prog));
            if (held >= 1.0) {
                m_opened = true;
                cbt::openPanel();
            }
        }
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!this->isVisible() || m_down || !this->getParent()) return false;
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        auto pos = this->getPosition();
        if (std::hypot(p.x - pos.x, p.y - pos.y) > this->radius()) return false;

        m_down = true;
        m_dragging = false;
        m_opened = false;
        m_downAt = cbt::nowSec();
        m_startTouch = p;
        m_startPos = pos;
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (!m_down) return;
        auto p = this->getParent()->convertTouchToNodeSpace(t);

        if (!m_dragging) {
            if (std::hypot(p.x - m_startTouch.x, p.y - m_startTouch.y) < 12.f) return;
            m_dragging = true;
            this->setScale(static_cast<float>(cbt::cfg.btnScale));
        }

        auto ws = CCDirector::sharedDirector()->getWinSize();
        float r = this->radius();
        float nx = std::clamp(m_startPos.x + (p.x - m_startTouch.x), r, ws.width - r);
        float ny = std::clamp(m_startPos.y + (p.y - m_startTouch.y), r, ws.height - r);
        this->setPosition({nx, ny});
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { this->finishTouch(); }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { this->finishTouch(); }
};

namespace cbt {
    inline void onChanged() {
        sanitize();
        saveCfg();
        applyFps();
        if (auto ov = OverlayManager::get()) {
            if (auto n = ov->getChildByID("cbt-button"_spr)) {
                static_cast<CBTButton*>(n)->refreshLook();
            }
        }
    }

    inline void ensureButton() {
        auto ov = OverlayManager::get();
        if (!ov) return;
        if (ov->getChildByID("cbt-button"_spr)) return;
        if (auto b = CBTButton::create()) ov->addChild(b, 9000);
    }
}

// ---------------------------------------------------------------------------
// Menu (category tabs, toggles, typed number inputs)
// ---------------------------------------------------------------------------
struct CBTRow {
    char const* label;
    bool* b;        // toggle (or nullptr)
    double* d;      // typed number (or nullptr)
    double lo;
    double hi;
    int action;     // 1 = reset button
};

struct CBTTab {
    char const* name;
    char const* note;
    std::vector<CBTRow> rows;
};

inline std::vector<CBTTab> const& cbtTabs() {
    static std::vector<CBTTab> t = {
        {"Input",
         "+ delays your clicks. - is the earliest the game\nallows (start of the frame). Sub-frame timing.",
         {
            {"Input Offset (ms)", &cbt::cfg.offsetOn, &cbt::cfg.offsetMs, -350, 350, 0},
            {"Click Sound", &cbt::cfg.clickSound, nullptr, 0, 0, 0},
         }},
        {"Click",
         "Hold To Click: clicks while you hold the screen.\nOff = clicks nonstop during a run.",
         {
            {"Auto Clicker (CPS)", &cbt::cfg.clickerOn, &cbt::cfg.cps, 1, 240, 0},
            {"Hold To Click", &cbt::cfg.clickerHold, nullptr, 0, 0, 0},
         }},
        {"Speed",
         "FPS changer is experimental and may do nothing\non some devices. TPS 240 = normal.",
         {
            {"Timescale", &cbt::cfg.timescaleOn, &cbt::cfg.timescale, 0.1, 10, 0},
            {"TPS Sub-step", &cbt::cfg.tpsOn, &cbt::cfg.tps, 240, 1000, 0},
            {"FPS Changer (Exp.)", &cbt::cfg.fpsOn, &cbt::cfg.fps, 30, 360, 0},
         }},
        {"Smooth",
         "Anti-Lag number = spike sensitivity (lower is\nstronger). Resync number = max drift in ms.",
         {
            {"Anti-Lag", &cbt::cfg.antiLag, &cbt::cfg.lagSens, 1.5, 8, 0},
            {"Music Resync (ms)", &cbt::cfg.musicSync, &cbt::cfg.syncMs, 20, 500, 0},
         }},
        {"Game",
         "Safe Mode marks runs as test mode while cheats\nare on, so nothing gets submitted.",
         {
            {"Auto Restart (sec)", &cbt::cfg.autoRestart, &cbt::cfg.restartDelay, 0, 3, 0},
            {"Anti-Kick", &cbt::cfg.antiKick, nullptr, 0, 0, 0},
            {"Safe Mode", &cbt::cfg.safeMode, nullptr, 0, 0, 0},
         }},
        {"Misc",
         "Hold the CBT circle for 1 second to open this\nmenu. Drag it to move it.",
         {
            {"Stats HUD", &cbt::cfg.showStats, nullptr, 0, 0, 0},
            {"Hide Button In Level", &cbt::cfg.hideBtnInLevel, nullptr, 0, 0, 0},
            {"Button Opacity", nullptr, &cbt::cfg.btnOpacity, 0.15, 1, 0},
            {"Button Size", nullptr, &cbt::cfg.btnScale, 0.5, 2, 0},
            {"Reset All Settings", nullptr, nullptr, 0, 0, 1},
         }},
    };
    return t;
}

class CBTPanel : public Popup {
protected:
    static constexpr float kW = 400.f;
    static constexpr float kH = 270.f;

    int m_tab = 0;
    float m_ox = 0.f;
    float m_oy = 0.f;
    CCNode* m_body = nullptr;
    CCLayerColor* m_marker = nullptr;
    std::vector<float> m_tabX;
    std::vector<float> m_tabW;

    bool init() {
        if (!Popup::init(kW, kH)) return false;
        this->setTitle("CBT Menu");

        auto cs = m_mainLayer->getContentSize();
        m_ox = (cs.width - kW) / 2.f;
        m_oy = (cs.height - kH) / 2.f;

        cbt::panelOpen = true;
        this->buildTabs();
        this->buildBody();
        return true;
    }

    void buildTabs() {
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setHandlerPriority(-504);
        m_mainLayer->addChild(menu, 6);

        auto const& T = cbtTabs();
        std::vector<ButtonSprite*> sprs;
        float sum = 0.f;
        for (auto const& tab : T) {
            auto spr = ButtonSprite::create(tab.name, "bigFont.fnt", "GJ_button_01.png", 0.5f);
            sprs.push_back(spr);
            sum += spr->getContentSize().width;
        }

        const float gap = 4.f;
        float avail = kW - 24.f - gap * static_cast<float>(T.size() - 1);
        float s = std::min(1.f, avail / sum);

        float total = sum * s + gap * static_cast<float>(T.size() - 1);
        float x = m_ox + (kW - total) / 2.f;
        float y = m_oy + kH - 52.f;

        for (size_t i = 0; i < T.size(); ++i) {
            sprs[i]->setScale(s);
            float w = sprs[i]->getContentSize().width * s;
            auto item = CCMenuItemSpriteExtra::create(
                sprs[i], nullptr, this, menu_selector(CBTPanel::onTab)
            );
            item->setTag(static_cast<int>(i));
            item->setPosition({x + w / 2.f, y});
            menu->addChild(item);
            m_tabX.push_back(x + w / 2.f);
            m_tabW.push_back(w);
            x += w + gap;
        }

        m_marker = CCLayerColor::create(ccc4(255, 220, 0, 255), 40.f, 3.f);
        m_mainLayer->addChild(m_marker, 6);
        this->moveMarker();
    }

    void moveMarker() {
        float w = m_tabW[m_tab] * 0.9f;
        m_marker->setContentSize({w, 3.f});
        m_marker->setPosition({m_tabX[m_tab] - w / 2.f, m_oy + kH - 52.f - 17.f});
    }

    void buildBody() {
        if (m_body) {
            m_body->removeFromParent();
            m_body = nullptr;
        }
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 5);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setHandlerPriority(-504);
        m_body->addChild(menu);

        auto const& tab = cbtTabs()[m_tab];
        float y = m_oy + kH - 96.f;
        int idx = 0;

        for (auto const& row : tab.rows) {
            auto lbl = CCLabelBMFont::create(row.label, "bigFont.fnt");
            lbl->setAnchorPoint({0.f, 0.5f});
            float sc = 0.45f;
            float maxW = 185.f;
            float lw = lbl->getContentSize().width;
            if (lw * sc > maxW) sc = maxW / lw;
            lbl->setScale(sc);
            lbl->setPosition({m_ox + 22.f, y});
            m_body->addChild(lbl);

            if (row.d) {
                auto input = TextInput::create(76.f, "0", "bigFont.fnt");
                input->setFilter("0123456789.-");
                input->setMaxCharCount(7);
                input->setString(fmt::format("{:g}", *row.d));
                input->setScale(0.8f);
                input->setPosition({m_ox + kW - 118.f, y});
                CBTRow r = row;
                input->setCallback([r](std::string const& s) {
                    if (s.empty() || s == "-" || s == ".") return;
                    char* end = nullptr;
                    double v = std::strtod(s.c_str(), &end);
                    if (end == s.c_str()) return;
                    *r.d = std::clamp(v, r.lo, r.hi);
                    cbt::onChanged();
                });
                m_body->addChild(input);
            }

            if (row.b) {
                auto tog = CCMenuItemToggler::createWithStandardSprites(
                    this, menu_selector(CBTPanel::onToggle), 0.55f
                );
                tog->setTag(idx);
                tog->toggle(*row.b);
                tog->setPosition({m_ox + kW - 38.f, y});
                menu->addChild(tog);
            }

            if (row.action == 1) {
                auto spr = ButtonSprite::create("Reset", "bigFont.fnt", "GJ_button_02.png", 0.5f);
                auto btn = CCMenuItemSpriteExtra::create(
                    spr, nullptr, this, menu_selector(CBTPanel::onReset)
                );
                btn->setPosition({m_ox + kW - 70.f, y});
                menu->addChild(btn);
            }

            y -= 32.f;
            ++idx;
        }

        auto note = CCLabelBMFont::create(tab.note, "chatFont.fnt");
        note->setScale(0.62f);
        note->setOpacity(190);
        note->setPosition({m_ox + kW / 2.f, m_oy + 26.f});
        m_body->addChild(note);
    }

    void onTab(CCObject* sender) {
        int i = static_cast<CCNode*>(sender)->getTag();
        if (i == m_tab || i < 0 || i >= static_cast<int>(cbtTabs().size())) return;
        m_tab = i;
        this->moveMarker();
        this->buildBody();
    }

    void onToggle(CCObject* sender) {
        auto t = static_cast<CCMenuItemToggler*>(sender);
        auto const& rows = cbtTabs()[m_tab].rows;
        int idx = t->getTag();
        if (idx < 0 || idx >= static_cast<int>(rows.size()) || !rows[idx].b) return;
        *rows[idx].b = !t->isToggled();
        cbt::onChanged();
    }

    void onReset(CCObject*) {
        double x = cbt::cfg.btnX;
        double y = cbt::cfg.btnY;
        cbt::cfg = cbt::Config{};
        cbt::cfg.btnX = x;
        cbt::cfg.btnY = y;
        cbt::onChanged();
        Notification::create("CBT settings reset", NotificationIcon::Success, 1.5f)->show();
        this->onClose(nullptr);
    }

public:
    static CBTPanel* create() {
        auto ret = new CBTPanel();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    ~CBTPanel() {
        cbt::panelOpen = false;
    }
};

void cbt::openPanel() {
    if (panelOpen) return;
    if (auto pl = PlayLayer::get()) {
        if (!pl->m_isPaused) pl->pauseGame(false);
    }
    if (auto p = CBTPanel::create()) p->show();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
class $modify(CBTBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (cbt::delivering || !cbt::inLevel(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Real player input from here on.
        if (button == 1) cbt::userHold = down;
        if (down) {
            cbt::registerClick();
            if (cbt::cfg.clickSound) {
                FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", 1.f, 0.f, 0.5f);
            }
        }

        if (!cbt::live()) {
            cbt::flushPending(this);
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // The auto clicker owns the jump button while it is on.
        if (cbt::cfg.clickerOn && button == 1) return;

        if (cbt::cfg.offsetOn) {
            cbt::pending.push_back({
                down, button, isPlayer1,
                cbt::nowSec() + cbt::cfg.offsetMs / 1000.0
            });
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        if (!cbt::inLevel(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }

        auto pl = PlayLayer::get();
        double now = cbt::nowSec();

        if (cbt::lastFrameReal > 0.0) {
            double real = now - cbt::lastFrameReal;
            if (real > 0.0005 && real < 1.0) {
                cbt::fpsEma = cbt::fpsEma * 0.92 + (1.0 / real) * 0.08;
            }
        }
        cbt::lastFrameReal = now;

        cbt::applyTimescale(pl);

        if (!cbt::live()) {
            cbt::flushPending(this);
            cbt::releaseSynth(this);
            cbt::accumulator = 0.0;
            GJBaseGameLayer::update(dt);
            cbt::hudTick(pl, now);
            return;
        }

        float d = cbt::lagAdjust(dt);

        bool stepping = cbt::cfg.tpsOn || cbt::cfg.offsetOn || cbt::cfg.clickerOn;
        if (!stepping) {
            cbt::releaseSynth(this);
            cbt::flushPending(this);
            cbt::accumulator = 0.0;
            GJBaseGameLayer::update(d);
            cbt::musicTick(pl, now);
            cbt::hudTick(pl, now);
            return;
        }

        const double step = 1.0 / (cbt::cfg.tpsOn ? cbt::cfg.tps : 240.0);
        cbt::accumulator += std::max(0.0f, d);

        // Real-time moment of the first sub-step of this frame.
        double t = now - cbt::accumulator;

        constexpr int kMaxSteps = 96;
        int n = 0;

        while (cbt::accumulator >= step && n < kMaxSteps) {
            cbt::clickerStep(this, step);

            while (!cbt::pending.empty() && cbt::pending.front().due <= t + 1e-6) {
                auto p = cbt::pending.front();
                cbt::pending.pop_front();
                cbt::deliver(this, p.down, p.button, p.p1);
            }

            GJBaseGameLayer::update(static_cast<float>(step));

            cbt::accumulator -= step;
            t += step;
            ++n;

            if (!cbt::live()) break;
        }

        // Drop any backlog we could not simulate so we never spiral.
        if (cbt::accumulator > step * 2.0) {
            cbt::accumulator = std::fmod(cbt::accumulator, step);
        }

        cbt::musicTick(pl, now);
        cbt::hudTick(pl, now);
    }
};

class $modify(CBTPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        cbt::resetRuntime();
        cbt::appliedScale = 1.f;
        cbt::lastFrameReal = 0.0;

        auto hud = CCLabelBMFont::create("", "bigFont.fnt");
        hud->setID("cbt-hud"_spr);
        hud->setScale(0.3f);
        hud->setOpacity(160);
        hud->setAnchorPoint({0.f, 1.f});
        auto ws = CCDirector::sharedDirector()->getWinSize();
        hud->setPosition({8.f, ws.height - 8.f});
        hud->setVisible(cbt::cfg.showStats);
        this->addChild(hud, 9999);

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        cbt::resetRuntime();
        cbt::appliedScale = 1.f;   // re-applied on the next update
    }

    void onExit() {
        if (cbt::appliedScale != 1.f) {
            this->updateTimeWarp(1.f);
            cbt::appliedScale = 1.f;
        }
        cbt::resetRuntime();
        PlayLayer::onExit();
    }

    void cbtRestart() {
        if (m_playerDied) this->resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        cbt::accumulator = 0.0;
        PlayLayer::destroyPlayer(player, object);

        if (cbt::cfg.autoRestart && m_playerDied) {
            constexpr int kRestartTag = 0xC8F1;
            this->stopActionByTag(kRestartTag);
            auto seq = CCSequence::create(
                CCDelayTime::create(static_cast<float>(cbt::cfg.restartDelay)),
                CCCallFunc::create(this, callfunc_selector(CBTPlayLayer::cbtRestart)),
                nullptr
            );
            seq->setTag(kRestartTag);
            this->runAction(seq);
        }
    }

    // Anti-Kick: ignore exits the player did not start from a menu.
    void onQuit() {
        if (cbt::cfg.antiKick && cbt::nowSec() > cbt::exitAllowedUntil) {
            Notification::create("Anti-Kick blocked a forced exit", NotificationIcon::Warning, 1.5f)->show();
            return;
        }
        PlayLayer::onQuit();
    }

    void levelComplete() {
        bool oldTest = m_isTestMode;
        if (cbt::cfg.safeMode && cbt::cheating()) m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = oldTest;
    }

    void showNewBest(
        bool newReward, int orbs, int diamonds,
        bool demonKey, bool noRetry, bool noTitle
    ) {
        if (cbt::cfg.safeMode && cbt::cheating()) return;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }
};

class $modify(CBTPause, PauseLayer) {
    void onQuit(CCObject* sender) {
        cbt::exitAllowedUntil = cbt::nowSec() + 20.0;
        PauseLayer::onQuit(sender);
    }
};

class $modify(CBTEnd, EndLevelLayer) {
    void onMenu(CCObject* sender) {
        cbt::exitAllowedUntil = cbt::nowSec() + 20.0;
        EndLevelLayer::onMenu(sender);
    }
};

class $modify(CBTMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        cbt::ensureButton();
        if (cbt::cfg.fpsOn) cbt::applyFps();
        return true;
    }
};

$on_mod(Loaded) {
    cbt::loadCfg();
    log::info("CBT v1.1 loaded");
}
