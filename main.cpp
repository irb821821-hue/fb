#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    using Tick = std::uint64_t;

    // TEST GUIDEBOOK ONLY. Replace with the target ticks for the actual level.
    // These are physics ticks since the current attempt began, at the user's
    // configured 240 ticks/second.
    constexpr std::array<Tick, 3> kGuidebookTargets = { 15, 43, 156 };

    // F6 cycle: OFF -> 2 -> 4 -> 8 -> 16 -> OFF.
    constexpr std::array<Tick, 5> kBufferStages = { 2, 4, 8, 16, 0 };
    std::size_t g_bufferStage = 4; // Start OFF so first F6 press activates 2 ticks.

    struct PendingInput {
        Tick dueTick;
        bool down;
        int button;
        bool isPlayer1;
        std::uint64_t sequence;
    };

    Tick g_tick = 0;
    std::size_t g_targetIndex = 0;
    std::uint64_t g_sequence = 0;
    std::vector<PendingInput> g_pending;
    bool g_dispatchingQueuedInput = false;

    // Per-player, per-button amount by which a buffered press was shifted.
    std::array<std::array<Tick, 16>, 2> g_holdShift{};

    // Debug HUD state; created under the active PlayLayer.
    PlayLayer* g_hudOwner = nullptr;
    cocos2d::CCLabelBMFont* g_hudLabel = nullptr;

    Tick activeBuffer() {
        return kBufferStages[g_bufferStage];
    }

    void resetAttemptState() {
        g_tick = 0;
        g_targetIndex = 0;
        g_sequence = 0;
        g_pending.clear();
        g_holdShift = {};
        g_dispatchingQueuedInput = false;
    }

    bool getButtonSlot(int button, bool isPlayer1, std::size_t& playerSlot, std::size_t& buttonSlot) {
        if (button < 0 || button >= static_cast<int>(g_holdShift[0].size())) {
            return false;
        }
        playerSlot = isPlayer1 ? 0u : 1u;
        buttonSlot = static_cast<std::size_t>(button);
        return true;
    }

    void skipExpiredTargets(Tick candidateTick) {
        while (g_targetIndex < kGuidebookTargets.size() &&
               kGuidebookTargets[g_targetIndex] < candidateTick) {
            ++g_targetIndex;
        }
    }

    std::string currentStageText() {
        const Tick buffer = activeBuffer();
        if (buffer == 0) return "OFF";
        return fmt::format("{}t", buffer);
    }

    void updateDebugHud(PlayLayer* layer) {
        if (!layer) return;

        if (g_hudOwner != layer || !g_hudLabel) {
            g_hudOwner = layer;
            g_hudLabel = nullptr;

            auto* director = cocos2d::CCDirector::sharedDirector();
            if (director) {
                auto winSize = director->getWinSize();
                g_hudLabel = cocos2d::CCLabelBMFont::create("PIB initializing...", "bigFont.fnt");
                if (g_hudLabel) {
                    g_hudLabel->setAnchorPoint({ 0.f, 1.f });
                    g_hudLabel->setPosition({ 8.f, winSize.height - 8.f });
                    g_hudLabel->setScale(0.32f);
                    g_hudLabel->setID("predictive-input-buffer-debug-hud");
                    layer->addChild(g_hudLabel, 10000);
                }
            }
        }

        if (!g_hudLabel) return;
        const auto nextTargetText = g_targetIndex < kGuidebookTargets.size()
            ? std::to_string(kGuidebookTargets[g_targetIndex])
            : std::string("done");
        const auto hudText = fmt::format(
            "PIB {} | tick {} | next {} | queued {}",
            currentStageText(), g_tick, nextTargetText, g_pending.size()
        );
        g_hudLabel->setString(hudText.c_str());
    }

    void showBufferNotification() {
        const Tick buffer = activeBuffer();
        const std::string message = buffer == 0
            ? "Predictive Input Buffer: OFF"
            : fmt::format("Predictive Input Buffer: {}-tick buffer", buffer);
        Notification::create(message, NotificationIcon::Info, 2.0f)->show();
        log::warn("{} (stage {}/5)", message, g_bufferStage + 1);
    }
}

// Listen to Geode's input event rather than hooking the Cocos keyboard
// dispatcher directly. This is also independent of whether a level is active.
$on_mod(Loaded) {
    log::warn("Predictive Input Buffer loaded. Starts OFF; press F6 to activate 2 ticks.");

    auto f6Listener = KeyboardInputEvent(cocos2d::enumKeyCodes::KEY_F6).listen(
        [](KeyboardInputData& data) {
            if (data.action != KeyboardInputData::Action::Press) {
                return;
            }

            // OFF -> 2 -> 4 -> 8 -> 16 -> OFF.
            g_bufferStage = (g_bufferStage + 1) % kBufferStages.size();
            showBufferNotification();
        },
        Priority::VeryEarly
    );
    f6Listener.leak();
}

class $modify(PredictiveInputBufferLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::VeryEarly);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_dispatchingQueuedInput) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto* playLayer = PlayLayer::get();
        auto* thisBase = static_cast<GJBaseGameLayer*>(this);
        if (!playLayer || static_cast<GJBaseGameLayer*>(playLayer) != thisBase ||
            !playLayer->m_started || playLayer->m_playerDied) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // handleButton arrives between command/physics steps. The next tick is
        // the tick on which an unmodified click would ordinarily be consumed.
        const Tick candidateTick = g_tick + 1;
        skipExpiredTargets(candidateTick);

        std::size_t playerSlot = 0;
        std::size_t buttonSlot = 0;
        const bool hasButtonSlot = getButtonSlot(button, isPlayer1, playerSlot, buttonSlot);

        // Keep a buffered release paired with its press to preserve hold length.
        if (!down) {
            if (hasButtonSlot) {
                const Tick shift = g_holdShift[playerSlot][buttonSlot];
                if (shift > 0) {
                    g_pending.push_back(PendingInput{
                        candidateTick + shift, false, button, isPlayer1, g_sequence++
                    });
                    g_holdShift[playerSlot][buttonSlot] = 0;
                    return;
                }
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (g_targetIndex >= kGuidebookTargets.size()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        const Tick target = kGuidebookTargets[g_targetIndex];
        const Tick buffer = activeBuffer();

        // At target tick: normal pass-through. A later click is also untouched.
        if (target <= candidateTick || buffer == 0) {
            if (target == candidateTick) ++g_targetIndex;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        const Tick delay = target - candidateTick;
        if (delay <= buffer) {
            ++g_targetIndex;
            g_pending.push_back(PendingInput{
                target, true, button, isPlayer1, g_sequence++
            });
            if (hasButtonSlot) {
                g_holdShift[playerSlot][buttonSlot] = delay;
            }
            log::info("Buffered early press: tick {} -> target {} ({} tick(s))",
                      candidateTick, target, delay);
            return;
        }

        // Too early for current stage: raw input remains unchanged.
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        auto* playLayer = PlayLayer::get();
        auto* thisBase = static_cast<GJBaseGameLayer*>(this);
        const bool isCurrentPlayLayer = playLayer &&
            static_cast<GJBaseGameLayer*>(playLayer) == thisBase;

        if (isCurrentPlayLayer && playLayer->m_started) {
            updateDebugHud(playLayer);

            if (playLayer->m_playerDied) {
                // Do not carry delayed inputs into a dead attempt/retry.
                g_pending.clear();
                g_holdShift = {};
            } else {
                ++g_tick;

                std::stable_sort(
                    g_pending.begin(), g_pending.end(),
                    [](PendingInput const& a, PendingInput const& b) {
                        if (a.dueTick != b.dueTick) return a.dueTick < b.dueTick;
                        return a.sequence < b.sequence;
                    }
                );

                while (!g_pending.empty() && g_pending.front().dueTick <= g_tick) {
                    const PendingInput input = g_pending.front();
                    g_pending.erase(g_pending.begin());

                    g_dispatchingQueuedInput = true;
                    this->handleButton(input.down, input.button, input.isPlayer1);
                    g_dispatchingQueuedInput = false;
                }
            }

            updateDebugHud(playLayer);
        }

        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(PredictiveInputBufferPlayLayer, PlayLayer) {
    void resetLevel() {
        resetAttemptState();
        PlayLayer::resetLevel();
    }

    void startGame() {
        resetAttemptState();
        PlayLayer::startGame();
        Notification::create(
            "Guidebook loaded: 3 example targets. F6 cycles input buffer.",
            NotificationIcon::Info,
            2.0f
        )->show();
    }
};
