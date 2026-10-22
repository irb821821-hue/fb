#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// This $modify block tells Geode: "We want to look inside Geometry Dash's PlayLayer (the actual gameplay screen)"
class $modify(BufferPlayLayer, PlayLayer) {
    
    // This function runs the moment a level starts
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        log::info("My Predictive Input Buffer mod has loaded successfully!");
        return true;
    }

    // This function triggers every single time you click or tap your mouse/keyboard
    void handleButton(bool down, int button, bool isPlayer1) {
        if (down) {
            // Right here is where you intercept the raw click!
            log::debug("Player clicked! Intercepting for timing adjustment.");
            
            // TODO: Add logic here to delay the click by 1-2 ticks if it's too early.
        }

        // This sends the click onward to the game engine
        PlayLayer::handleButton(down, button, isPlayer1);
    }
};
