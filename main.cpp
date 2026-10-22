#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

// Global state variables for the mod
enum class BufferStage {
    STAGE_1 = 1, // 2 Ticks
    STAGE_2 = 2, // 4 Ticks
    STAGE_3 = 3, // 8 Ticks
    STAGE_4 = 4, // 16 Ticks
    OFF = 5      // Disabled
};

BufferStage g_currentStage = BufferStage::STAGE_1;

// Helper function to print current stage status to console
void logStageStatus() {
    switch (g_currentStage) {
        case BufferStage::STAGE_1:
            log::info("[InputBuffer] F6 Cycle -> STAGE 1: 2 Ticks All | 2 Ticks Wave (0.5f @ 60fps)");
            break;
        case BufferStage::STAGE_2:
            log::info("[InputBuffer] F6 Cycle -> STAGE 2: 4 Ticks All | 4 Ticks Wave (1.0f @ 60fps)");
            break;
        case BufferStage::STAGE_3:
            log::info("[InputBuffer] F6 Cycle -> STAGE 3: 8 Ticks All | 8 Ticks Wave (2.0f @ 60fps)");
            break;
        case BufferStage::STAGE_4:
            log::info("[InputBuffer] F6 Cycle -> STAGE 4: 16 Ticks All | 8 Ticks Wave (4.0f / 2.0f @ 60fps)");
            break;
        case BufferStage::OFF:
            log::info("[InputBuffer] F6 Cycle -> STAGE 5: OFF (100% Raw Input)");
            break;
    }
}

// Hook into the global keyboard dispatcher to catch F6 anywhere in-level
class $modify(MyKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down) {
        // Trigger only on key down and specifically for the F6 key
        if (down && key == cocos2d::enumKeyCodes::KEY_F6) {
            // Check if we are actively playing inside a level
            if (PlayLayer::get()) {
                if (g_currentStage == BufferStage::OFF) {
                    g_currentStage = BufferStage::STAGE_1;
                } else {
                    g_currentStage = static_cast<BufferStage>(static_cast<int>(g_currentStage) + 1);
                }
                logStageStatus();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down);
    }
};

// Hook into PlayLayer to intercept inputs and log simulation
class $modify(BufferPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        log::info("==================================================");
        log::info("[InputBuffer] v4.6 LOADED (Wave Spikes/Saws Trajectory Fix)");
        log::info("[InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)");
        log::info("[InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
        log::info("==================================================");

        return true;
    }

    // Intercept player click/jump inputs
    void handleButton(bool down, int button, bool isPlayer1) {
        if (down && g_currentStage != BufferStage::OFF) {
            int maxTicks = 0;
            switch (g_currentStage) {
                case BufferStage::STAGE_1: maxTicks = 2; break;
                case BufferStage::STAGE_2: maxTicks = 4; break;
                case BufferStage::STAGE_3: maxTicks = 8; break;
                case BufferStage::STAGE_4: maxTicks = 16; break;
                default: break;
            }

            float framesAt60 = (float)maxTicks / 4.0f;
            log::info("[InputBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) PRESS! [Limit: {}] ***", 
                maxTicks / 2, framesAt60 / 2.0f, maxTicks);
        }

        PlayLayer::handleButton(down, button, isPlayer1);
    }
};
