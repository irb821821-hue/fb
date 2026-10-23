#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace PIB {
    // Example guidebook target ticks. These are 1-based ticks from the start
    // of an attempt. Replace these values with your level's real guidebook.
    constexpr std::array<int, 14> kGuidebookTargets = {
        333, 514, 1050, 1721, 2027, 2174, 3442,
        3490, 4095, 4226, 5090, 5268, 5495, 5785
    };

    // F6 cycles: 2 -> 4 -> 8 -> 16 -> 32 -> OFF -> 2 ...
    constexpr std::array<int, 6> kBufferStages = { 2, 4, 8, 16, 32, 0 };
    int g_bufferStage = 0; // starts at 2 ticks

    // Count calls to GJBaseGameLayer::processCommands while the real PlayLayer
    // is active. On the target GD build this is used as the physics-step clock.
    int g_physicsTick = 0;
    std::size_t g_targetIndex = 0;

    struct PendingInput {
        int targetTick;
        bool down;
        int button;
        bool isPlayer1;
    };

    std::vector<PendingInput> g_pendingInputs;

    // If a press is shifted forward, shift its matching release by the same
    // number of ticks to preserve the original hold duration.
    // Key: {button id, isPlayer1}; value: input delay in ticks.
    std::map<std::pair<int, bool>, int> g_buttonDelayTicks;

    CCLabelBMFont* g_hud = nullptr;

    int bufferTicks() {
        return kBufferStages[g_bufferStage];
    }

    std::string bufferText() {
        const int ticks = bufferTicks();
        return ticks == 0 ? std::string("OFF") : std::to_string(ticks) + " ticks";
    }

    int nextTarget() {
        if (g_targetIndex >= kGuidebookTargets.size()) {
            return -1;
        }
        return kGuidebookTargets[g_targetIndex];
    }

    void resetAttempt() {
        g_physicsTick = 0;
        g_targetIndex = 0;
        g_pendingInputs.clear();
        g_buttonDelayTicks.clear();
    }

    void showStatus(const std::string& message) {
        log::warn("[PIB] {}", message);
        if (auto notification = Notification::create(
                message,
                NotificationIcon::Info,
                2.5f
            )) {
            notification->show();
        }
    }

    void cycleBuffer() {
        g_bufferStage = (g_bufferStage + 1) % static_cast<int>(kBufferStages.size());
        showStatus("Buffer: " + bufferText());
    }

    void updateHud() {
        if (!g_hud) {
            return;
        }

        const int target = nextTarget();
        const std::string targetText = target < 0 ? "done" : std::to_string(target);
        const std::string message =
            "PIB | buffer: " + bufferText() +
            " | tick: " + std::to_string(g_physicsTick) +
            " | target: " + targetText +
            " | queued: " + std::to_string(g_pendingInputs.size());

        g_hud->setString(message.c_str());
    }

    void advancePastMissedTargets(int tick) {
        while (g_targetIndex < kGuidebookTargets.size() &&
               kGuidebookTargets[g_targetIndex] < tick) {
            ++g_targetIndex;
        }
    }

    void sortPendingInputs() {
        std::stable_sort(
            g_pendingInputs.begin(),
            g_pendingInputs.end(),
            [](PendingInput const& a, PendingInput const& b) {
                return a.targetTick < b.targetTick;
            }
        );
    }
}

// Keyboard hook: F6 cycles the buffer stage and displays a notification.
class $modify(PIBKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(
        cocos2d::enumKeyCodes key,
        bool isKeyDown,
        bool isKeyRepeat
    ) {
        const bool result = CCKeyboardDispatcher::dispatchKeyboardMSG(
            key, isKeyDown, isKeyRepeat
        );

        if (key == cocos2d::enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat) {
            PIB::cycleBuffer();
        }

        return result;
    }
};

// Game hook: delay qualifying early jump presses until their guidebook target.
class $modify(PIBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* playLayer = PlayLayer::get();

        // Only alter actual gameplay, and only the standard jump button.
        if (!playLayer || static_cast<GJBaseGameLayer*>(playLayer) != this ||
            !playLayer->m_started || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        const auto buttonKey = std::make_pair(button, isPlayer1);
        const int candidateTick = PIB::g_physicsTick + 1;

        // Shift the release by the same amount as a previously buffered press.
        if (!down) {
            auto it = PIB::g_buttonDelayTicks.find(buttonKey);
            if (it != PIB::g_buttonDelayTicks.end()) {
                const int delay = it->second;
                PIB::g_buttonDelayTicks.erase(it);
                if (delay > 0) {
                    PIB::g_pendingInputs.push_back({
                        candidateTick + delay, false, button, isPlayer1
                    });
                    return;
                }
            }

            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // If the guidebook target has already passed, it cannot be corrected.
        PIB::advancePastMissedTargets(candidateTick);
        const int targetTick = PIB::nextTarget();

        if (targetTick >= 0 && targetTick == candidateTick) {
            ++PIB::g_targetIndex;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Only early presses inside the selected buffer are moved to the target.
        if (targetTick > candidateTick) {
            const int delay = targetTick - candidateTick;
            const int maxEarlyTicks = PIB::bufferTicks();

            if (delay > 0 && delay <= maxEarlyTicks) {
                ++PIB::g_targetIndex;
                PIB::g_buttonDelayTicks[buttonKey] = delay;
                PIB::g_pendingInputs.push_back({
                    targetTick, true, button, isPlayer1
                });
                log::info(
                    "[PIB] buffered press at tick {} for target {} (delay {})",
                    candidateTick, targetTick, delay
                );
                return;
            }
        }

        // Late or out-of-range inputs remain raw.
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        auto* playLayer = PlayLayer::get();
        const bool isActivePlayLayer =
            playLayer && static_cast<GJBaseGameLayer*>(playLayer) == this &&
            playLayer->m_started;

        if (isActivePlayLayer) {
            ++PIB::g_physicsTick;
            PIB::advancePastMissedTargets(PIB::g_physicsTick);
            PIB::sortPendingInputs();

            // Dispatch queued input right before the game processes this step.
            auto it = PIB::g_pendingInputs.begin();
            while (it != PIB::g_pendingInputs.end()) {
                if (it->targetTick <= PIB::g_physicsTick) {
                    auto event = *it;
                    it = PIB::g_pendingInputs.erase(it);
                    // Calling the base implementation here bypasses this hook,
                    // so the queued event is not buffered a second time.
                    GJBaseGameLayer::handleButton(
                        event.down, event.button, event.isPlayer1
                    );
                } else {
                    ++it;
                }
            }
        }

        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(PIBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        const bool result = PlayLayer::init(level, useReplay, dontCreateObjects);
        if (!result) {
            return false;
        }

        PIB::resetAttempt();

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto* label = CCLabelBMFont::create("PIB loading...", "bigFont.fnt");
        if (label) {
            label->setAnchorPoint({ 0.f, 1.f });
            label->setScale(0.38f);
            label->setPosition({ 8.f, winSize.height - 8.f });
            this->addChild(label, 999999);
            PIB::g_hud = label;
            PIB::updateHud();
        }

        log::info("[PIB] PlayLayer initialized; guidebook has {} targets",
                  PIB::kGuidebookTargets.size());
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        PIB::updateHud();
    }

    void resetLevel() {
        PIB::resetAttempt();
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        PIB::resetAttempt();
        PlayLayer::resetLevelFromStart();
    }
};

$on_mod(Loaded) {
    log::warn("[PIB] Predictive Input Buffer loaded. Press F6 to cycle 2/4/8/16/OFF.");
    if (auto notification = Notification::create(
            "PIB loaded - press F6 to change buffer",
            NotificationIcon::Info,
            4.0f
        )) {
        notification->show();
    }
}
