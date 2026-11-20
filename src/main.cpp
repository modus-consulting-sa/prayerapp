#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

static bool isAutoEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

class $modify(AutoPlayLayer, PlayLayer) {
    struct Fields {
        // True if the cheat was active at any point during the current attempt.
        bool m_cheatedThisAttempt = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        m_fields->m_cheatedThisAttempt = isAutoEnabled();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_cheatedThisAttempt = isAutoEnabled();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The anti-cheat spike must always go through the original code,
        // otherwise the game flags the attempt and can misbehave.
        if (isAutoEnabled() && object != m_anticheatSpike) {
            m_fields->m_cheatedThisAttempt = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (!m_fields->m_cheatedThisAttempt && !isAutoEnabled()) {
            PlayLayer::levelComplete();
            return;
        }
        // Safe mode: treat the completion like a start-pos/test run so no
        // progress, stars, moons, orbs or coins are saved or uploaded.
        bool oldTestMode = m_isTestMode;
        m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = oldTestMode;
    }
};

class $modify(AutoPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("pause-button")) return;

        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("left-button-menu"));
        bool ownMenu = false;
        if (!menu) {
            // Fallback if node IDs are unavailable: our own small menu, top-left.
            menu = CCMenu::create();
            if (!menu) return;
            auto win = CCDirector::get()->getWinSize();
            menu->setPosition({35.f, win.height - 35.f});
            menu->setID("auto-complete-menu"_spr);
            this->addChild(menu, 10);
            ownMenu = true;
        }

        auto toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AutoPauseLayer::onToggleAuto), 0.7f
        );
        if (!toggler) return;
        toggler->toggle(isAutoEnabled());
        toggler->setID("auto-complete-toggle"_spr);

        auto label = CCLabelBMFont::create("Auto", "bigFont.fnt");
        label->setScale(0.35f);
        label->setPosition({toggler->getContentWidth() / 2.f, -6.f});
        toggler->addChild(label);

        menu->addChild(toggler);
        if (!ownMenu) menu->updateLayout();
    }

    void onToggleAuto(CCObject* sender) {
        // CCMenuItemToggler fires before flipping its own state.
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        bool newValue = !toggler->isToggled();
        Mod::get()->setSettingValue<bool>("enabled", newValue);
        if (newValue) {
            if (auto pl = PlayLayer::get()) {
                static_cast<AutoPlayLayer*>(pl)->m_fields->m_cheatedThisAttempt = true;
            }
        }
    }
};
