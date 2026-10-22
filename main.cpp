#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {
    using Tick = std::uint64_t;

    // Example guidebook. Replace this list with the guidebook's target ticks.
    // Ticks are 1-based physics ticks from the start of the attempt (240 ticks/sec).
    constexpr std::array<Tick, 3> kGuidebookTargets = { 240, 480, 720 };

    // F6 cycles: 2, 4, 8, 16 ticks, then OFF.
    constexpr std::array<Tick, 5> kBufferStages = { 2, 4, 8, 16, 0 };
    std::size_t g_bufferStage = 0; // Start at stage 1: 2 ticks.

    struct PendingInput {
        Tick dueTick;
        bool down;
        int button;
        bool isPlayer1;
        std::uint64_t sequence;
    };

    Tick g_tick = 0; // Most recently entered physics tick.
    std::size_t g_targetIndex = 0;
    std::uint64_t g_sequence = 0;
    std::vector<PendingInput> g_pending;
    bool g_dispatchingQueuedInput = false;

    // A buffered press shifts its matching release by the same amount, keeping
    // the original hold duration. Geometry Dash button values are small ints;
    // guard the index so unknown values safely pass through raw.
    std::array<std::array<Tick, 16>, 2> g_holdShift{};

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
}

class $modify(PredictiveInputBufferLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void) self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        (void) self.setHookPriority("GJBaseGameLayer::processCommands", Priority::VeryEarly);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Queued inputs are sent back through this hook; let those reach GD's
        // original handler without being matched against the guidebook again.
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

        // handleButton is received between physics steps. The next step is the
        // candidate tick on which an unmodified input would be consumed.
        const Tick candidateTick = g_tick + 1;
        skipExpiredTargets(candidateTick);

        std::size_t playerSlot = 0;
        std::size_t buttonSlot = 0;
        const bool hasButtonSlot = getButtonSlot(button, isPlayer1, playerSlot, buttonSlot);

        // If this press was buffered, shift the matching release by the same
        // delta. This also handles a release arriving before the queued press.
        if (!down) {
            if (hasButtonSlot) {
                const Tick shift = g_holdShift[playerSlot][buttonSlot];
                if (shift > 0) {
                    g_pending.push_back(PendingInput {
                        candidateTick + shift,
                        false,
                        button,
                        isPlayer1,
                        g_sequence++
                    });
                    g_holdShift[playerSlot][buttonSlot] = 0;
                    return;
                }
            }

            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Exact target: don't delay; let the engine receive the click normally.
        if (g_targetIndex < kGuidebookTargets.size() &&
            kGuidebookTargets[g_targetIndex] == candidateTick) {
            ++g_targetIndex;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        const Tick target = g_targetIndex < kGuidebookTargets.size()
            ? kGuidebookTargets[g_targetIndex]
            : 0;
        const Tick buffer = kBufferStages[g_bufferStage];

        // Early press inside the selected window: hold it until the guidebook
        // target tick. Late inputs and inputs outside the window remain raw.
        if (target > candidateTick && (target - candidateTick) <= buffer) {
            const Tick shift = target - candidateTick;
            ++g_targetIndex;

            g_pending.push_back(PendingInput {
                target,
                true,
                button,
                isPlayer1,
                g_sequence++
            });

            if (hasButtonSlot) {
                g_holdShift[playerSlot][buttonSlot] = shift;
            }
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        auto* playLayer = PlayLayer::get();
        auto* thisBase = static_cast<GJBaseGameLayer*>(this);
        const bool isCurrentPlayLayer = playLayer &&
            static_cast<GJBaseGameLayer*>(playLayer) == thisBase;

        if (isCurrentPlayLayer && playLayer->m_started) {
            if (playLayer->m_playerDied) {
                // Never allow delayed inputs from a dead attempt to leak into a retry.
                g_pending.clear();
                g_holdShift = {};
            } else {
                // In GD 2.2074, processCommands is part of the physics-step path.
                // Advance once, then inject due inputs before the original command
                // processing consumes the button queue for this physics tick.
                ++g_tick;
                std::stable_sort(
                    g_pending.begin(), g_pending.end(),
                    [](const PendingInput& a, const PendingInput& b) {
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
    }
};

class $modify(PredictiveInputBufferKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        auto* playLayer = PlayLayer::get();
        if (key == cocos2d::enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat &&
            playLayer && playLayer->m_started && !playLayer->m_playerDied) {
            g_bufferStage = (g_bufferStage + 1) % kBufferStages.size();
            const Tick buffer = kBufferStages[g_bufferStage];
            if (buffer == 0) {
                log::info("Predictive Input Buffer: stage {}/5 — OFF", g_bufferStage + 1);
            } else {
                log::info("Predictive Input Buffer: stage {}/5 — {} tick buffer", g_bufferStage + 1, buffer);
            }
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

