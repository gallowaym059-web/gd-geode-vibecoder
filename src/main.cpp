#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

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
    static CCLabelBMFont* hud = nullptr;

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

    static void hudUpdate() {
        if (!hud)
            return;

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

    static double nowSeconds() {
        using clock = std::chrono::steady_clock;

        static const auto start = clock::now();

        return std::chrono::duration<double>(
            clock::now() - start
        ).count();
    }
}

// CBT input timing layer.
class $modify(CBTInputLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool player2) {
        if (!cbt::active(this)) {
            GJBaseGameLayer::handleButton(
                down,
                button,
                player2
            );
            return;
        }

        GJBaseGameLayer::handleButton(
            down,
            button,
            player2
        );

        auto pl = PlayLayer::get();

        if (!pl || pl->m_queuedButtons.empty())
            return;

        auto& cmd = pl->m_queuedButtons.back();

        const double bias =
            static_cast<double>(cbt::precisionMs) / 1000.0;

        cmd.m_timestamp += bias;
    }
}

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
        if (
            !PlayLayer::init(
                level,
                useReplay,
                dontCreateObjects
            )
        ) {
            return false;
        }

        cbt::hud = CCLabelBMFont::create(
            "",
            "bigFont.fnt"
        );

        cbt::hud->setScale(0.30f);
        cbt::hud->setOpacity(155);
        cbt::hud->setAnchorPoint({1.f, 1.f});

        auto size =
            CCDirector::sharedDirector()->getWinSize();

        cbt::hud->setPosition({
            size.width - 8.f,
            size.height - 8.f
        });

        addChild(cbt::hud, 9999);

        cbt::hudUpdate();

        return true;
    }

    void destroyPlayer(
        PlayerObject* player,
        GameObject* object
    ) {
        cbt::accumulator = 0.0;

        PlayLayer::destroyPlayer(
            player,
            object
        );
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
        bool p0,
        int p1,
        int p2,
        bool p3,
        bool p4,
        bool p5
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
            p0,
            p1,
            p2,
            p3,
            p4,
            p5
        );
    }
};

$on_mod(Loaded) {
    cbt::refresh();

    listenForSettingChanges(
        "enabled",
        [](bool value) {
            cbt::enabled = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges(
        "extrapolate",
        [](bool value) {
            cbt::extrapolate = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges(
        "show-overlay",
        [](bool value) {
            cbt::overlay = value;
            cbt::hudUpdate();
        }
    );

    listenForSettingChanges(
        "safe-mode",
        [](bool value) {
            cbt::safeMode = value;
        }
    );

    listenForSettingChanges(
        "precision-ms",
        [](int64_t value) {
            cbt::precisionMs =
                std::clamp<int64_t>(
                    value,
                    -65,
                    65
                );

            cbt::hudUpdate();
        }
    );

    listenForSettingChanges(
        "tps",
        [](int64_t value) {
            cbt::targetTPS =
                std::clamp<int64_t>(
                    value,
                    240,
                    1000
                );

            cbt::accumulator = 0.0;
            cbt::hudUpdate();
        }
    );

    log::info(
        "CBT v1.0 Alpha loaded"
    );
}
