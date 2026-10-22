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
    static void onModify(auto& self) {
        (void) self.setHookPriority("CCKeyboardDispatcher::dispatchKeyboardMSG", Priority::VeryEarly);
        log::warn("Predictive Input Buffer: keyboard hook initialized");
    }

    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        // Log and show an in-game notification for F6 even outside gameplay.
        // This distinguishes an input-hook issue from the old gameplay-only condition.
        if (key == cocos2d::enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat) {
            auto* playLayer = PlayLayer::get();
            if (!playLayer || !playLayer->m_started || playLayer->m_playerDied) {
                log::warn("Predictive Input Buffer: F6 detected, but no active level is running");
                Notification::create(
                    "Input Buffer: F6 detected (start a level to cycle)",
                    NotificationIcon::Info,
                    2.0f
                )->show();
            } else {
                g_bufferStage = (g_bufferStage + 1) % kBufferStages.size();
                const Tick buffer = kBufferStages[g_bufferStage];
                if (buffer == 0) {
                    log::warn("Predictive Input Buffer: stage {}/5 — OFF", g_bufferStage + 1);
                    Notification::create("Input Buffer: OFF", NotificationIcon::Info, 2.0f)->show();
                } else {
                    log::warn("Predictive Input Buffer: stage {}/5 — {} tick buffer", g_bufferStage + 1, buffer);
                    Notification::create(
                        fmt::format("Input Buffer: stage {}/5 — {} ticks", g_bufferStage + 1, buffer),
                        NotificationIcon::Info,
                        2.0f
                    )->show();
                }
            }
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
