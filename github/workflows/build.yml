#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

namespace cbt {
    static bool enabled = true;
    static bool extrapolate = false;
    static bool overlay = true;
    static bool safeMode = true;
    static int64_t precisionMs = 0;
    static int64_t targetTPS = 240;
    static double accumulator = 0.0;
    static bool substepping = false;

    static void refresh() {
        auto m = Mod::get();
        enabled = m->getSettingValue<bool>("enabled");
        extrapolate = m->getSettingValue<bool>("extrapolate");
        overlay = m->getSettingValue<bool>("show-overlay");
        safeMode = m->getSettingValue<bool>("safe-mode");
        precisionMs = std::clamp<int64_t>(
            m->getSettingValue<int64_t>("precision-ms"), -65, 65
        );
        targetTPS = std::clamp<int64_t>(
            m->getSettingValue<int64_t>("tps"), 240, 1000
        );
    }

    static bool active(GJBaseGameLayer* layer) {
        auto pl = PlayLayer::get();

        return enabled &&
               pl &&
               layer == static_cast<GJBaseGameLayer*>(pl) &&
               pl->m_started &&
               !pl->m_playerDied;
    }

    // The overlay label is looked up by node ID instead of being cached in a
    // static pointer. A cached pointer dangles once the PlayLayer is destroyed,
    // and the settings callbacks below can fire at any time (e.g. from the
    // mod settings menu), which would crash.
    static void hudUpdate(PlayLayer* pl = nullptr) {
        if (!pl) pl = PlayLayer::get();
        if (!pl) return;

        auto hud = typeinfo_cast<CCLabelBMFont*>(
            pl->getChildByID("cbt-hud"_spr)
        );
        if (!hud) return;

        hud->setVisible(overlay && enabled);

        if (!overlay || !enabled)
            return;

        hud->setString(
            fmt::format(
                "CBT  {}ms  {} TPS{}",
                precisionMs,
                targetTPS,
                extrapolate ? "  EX" : ""
            ).c_str()
        );
    }
}

// CBT input timing layer.
class $modify(CBTInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!cbt::active(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto const before = m_queuedButtons.size();

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // Only bias a command that this call actually queued.
        if (m_queuedButtons.size() <= before)
            return;

        const double bias =
            static_cast<double>(cbt::precisionMs) / 1000.0;

        m_queuedButtons.back().m_timestamp += bias;
    }
}; // <-- this semicolon was missing

// Experimental TPS bypass.
class $modify(CBTPhysicsLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (
            !cbt::active(this) ||
            cbt::targetTPS <= 240 ||
            cbt::substepping
        ) {
            GJBaseGameLayer::update(dt);
            cbt::hudUpdate();
            return;
        }

        cbt::accumulator += std::max(0.0f, dt);

        const double step =
            1.0 / static_cast<double>(cbt::targetTPS);

        constexpr int MAX_SUBSTEPS = 64;

        int steps = 0;

        cbt::substepping = true;

        while (
            cbt::accumulator >= step &&
            steps < MAX_SUBSTEPS
        ) {
            GJBaseGameLayer::update(
                static_cast<float>(step)
            );

            cbt::accumulator -= step;
            ++steps;

            if (
                !PlayLayer::get() ||
                PlayLayer::get()->m_playerDied
            ) {
                break;
            }
        }

        cbt::substepping = false;

        cbt::accumulator = std::min(
            cbt::accumulator,
            step * MAX_SUBSTEPS
        );

        cbt::hudUpdate();
    }
};

class $modify(CBTPlayLayer, PlayLayer) {
    bool init(
        GJGameLevel* level,
        bool useReplay,
        bool dontCreateObjects
    ) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        auto hud = CCLabelBMFont::create("", "bigFont.fnt");
        hud->setID("cbt-hud"_spr);
        hud->setScale(0.30f);
        hud->setOpacity(155);
        hud->setAnchorPoint({1.f, 1.f});

        auto size = CCDirector::sharedDirector()->getWinSize();
        hud->setPosition({size.width - 8.f, size.height - 8.f});

        this->addChild(hud, 9999);

        cbt::hudUpdate(this);

        return true;
    }

    void destroyPlayer(
        PlayerObject* player,
        GameObject* object
    ) {
        cbt::accumulator = 0.0;

        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        bool oldTest = m_isTestMode;

        if (
            cbt::safeMode &&
            (
                cbt::precisionMs != 0 ||
                cbt::targetTPS != 240 ||
                cbt::extrapolate
            )
        ) {
            m_isTestMode = true;
        }

        PlayLayer::levelComplete();

        m_isTestMode = oldTest;
    }

    void showNewBest(
        bool newReward,
        int orbs,
        int diamonds,
        bool demonKey,
        bool noRetry,
        bool noTitle
    ) {
        if (
            cbt::safeMode &&
            (
                cbt::precisionMs != 0 ||
                cbt::targetTPS != 240 ||
                cbt::extrapolate
            )
        ) {
            return;
        }

        PlayLayer::showNewBest(
            newReward,
            orbs,
            diamonds,
            demonKey,
            noRetry,
            noTitle
        );
    }
};

$on_mod(Loaded) {
    cbt::refresh();

    // Geode 5.x: the value type is the first template parameter and cannot be
    // deduced from the lambda, so it must be written out explicitly.
    listenForSettingChanges<bool>(
        "enabled",
        [](bool value) {
            cbt::enabled = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges<bool>(
        "extrapolate",
        [](bool value) {
            cbt::extrapolate = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges<bool>(
        "show-overlay",
        [](bool value) {
            cbt::overlay = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges<bool>(
        "safe-mode",
        [](bool value) {
            cbt::safeMode = value;
        }
    );

    listenForSettingChanges<int64_t>(
        "precision-ms",
        [](int64_t value) {
            cbt::precisionMs = std::clamp<int64_t>(value, -65, 65);
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges<int64_t>(
        "tps",
        [](int64_t value) {
            cbt::targetTPS = std::clamp<int64_t>(value, 240, 1000);
            cbt::accumulator = 0.0;
            cbt::hudUpdate();
        }
    );

    log::info("CBT v1.0 Alpha loaded");
}
