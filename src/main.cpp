#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static bool isNoclipEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

static bool isBotEnabled() {
    return Mod::get()->getSettingValue<bool>("bot");
}

// ---------------------------------------------------------------------------
// Bot helpers
// ---------------------------------------------------------------------------

namespace {
    enum class Kind { Hazard, Solid, Slope, Ring };

    struct Entry {
        float x;
        GameObject* obj;
        Kind kind;
    };

    bool isRingType(GameObjectType t) {
        switch (t) {
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SpiderOrb:
                return true;
            default:
                return false;
        }
    }

    bool isUsable(GameObject* obj) {
        return obj && !obj->m_isNoTouch && !obj->m_isPassable && !obj->m_isGroupDisabled;
    }

    bool overlaps(float aMin, float aMax, float bMin, float bMax) {
        return aMin < bMax && bMin < aMax;
    }
}

// ---------------------------------------------------------------------------
// PlayLayer: noclip, safe mode, bot state
// ---------------------------------------------------------------------------

class $modify(AutoPlayLayer, PlayLayer) {
    struct Fields {
        // True if any cheat was active at any point during the current attempt.
        bool m_cheatedThisAttempt = false;

        // Bot state
        std::vector<Entry> m_entries;
        bool m_built = false;
        bool m_holding = false;
        CCLabelBMFont* m_botLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto f = m_fields.self();
        f->m_cheatedThisAttempt = isNoclipEnabled() || isBotEnabled();

        // Small on-screen marker while the bot is playing.
        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("Bot", "bigFont.fnt");
        if (label) {
            label->setScale(0.3f);
            label->setOpacity(140);
            label->setAnchorPoint({0.f, 1.f});
            label->setPosition({5.f, win.height - 5.f});
            label->setID("bot-label"_spr);
            label->setVisible(isBotEnabled());
            this->addChild(label, 1000);
            f->m_botLabel = label;
        }
        return true;
    }

    void resetLevel() {
        auto f = m_fields.self();
        if (f->m_holding) {
            f->m_holding = false;
            this->handleButton(false, 1, true);
        }
        PlayLayer::resetLevel();
        f->m_cheatedThisAttempt = isNoclipEnabled() || isBotEnabled();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The anti-cheat spike must always go through the original code.
        if (isNoclipEnabled() && object != m_anticheatSpike) {
            m_fields->m_cheatedThisAttempt = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto f = m_fields.self();
        if (!f->m_cheatedThisAttempt && !isNoclipEnabled() && !isBotEnabled()) {
            PlayLayer::levelComplete();
            return;
        }
        // Safe mode: treat the completion like a start-pos/test run so no
        // progress, stars, moons, orbs, diamonds or coins are saved or uploaded.
        bool oldTestMode = m_isTestMode;
        m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = oldTestMode;
    }

    // ---- bot ---------------------------------------------------------------

    void buildEntries() {
        auto f = m_fields.self();
        if (f->m_built || !m_objects || m_objects->count() == 0) return;

        f->m_entries.clear();
        f->m_entries.reserve(m_objects->count() / 4);
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            auto t = obj->m_objectType;
            Kind kind;
            if (t == GameObjectType::Hazard) kind = Kind::Hazard;
            else if (t == GameObjectType::Solid) kind = Kind::Solid;
            else if (t == GameObjectType::Slope) kind = Kind::Slope;
            else if (isRingType(t)) kind = Kind::Ring;
            else continue;
            f->m_entries.push_back({obj->getPositionX(), obj, kind});
        }
        std::sort(f->m_entries.begin(), f->m_entries.end(),
            [](Entry const& a, Entry const& b) { return a.x < b.x; });
        f->m_built = true;
    }

    template <class F>
    void forEachNear(float xMin, float xMax, F&& fn) {
        auto& entries = m_fields->m_entries;
        // Margin covers object width and small trigger-driven movement.
        constexpr float margin = 150.f;
        auto it = std::lower_bound(entries.begin(), entries.end(), xMin - margin,
            [](Entry const& e, float x) { return e.x < x; });
        for (; it != entries.end() && it->x <= xMax + margin; ++it) {
            if (!isUsable(it->obj)) continue;
            fn(*it, it->obj->getObjectRect());
        }
    }

    void setHold(bool down) {
        auto f = m_fields.self();
        if (f->m_holding == down) return;
        f->m_holding = down;
        this->handleButton(down, 1, true);
    }

    bool decide(PlayerObject* p) {
        auto f = m_fields.self();
        CCRect pr = p->getObjectRect();

        float g = p->m_isUpsideDown ? -1.f : 1.f;   // gravity frame
        float dir = p->m_isGoingLeft ? -1.f : 1.f;  // movement direction
        float s = std::max(0.6f, p->m_playerSpeed / 0.9f);
        float py = pr.getMidY();
        float front = dir > 0 ? pr.getMaxX() : pr.getMinX();
        float vel = static_cast<float>(p->m_yVelocity);

        // ---- flying modes: steer toward the middle of the free gap ahead ----
        bool flying = p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
        if (flying) {
            float look = 110.f * s;
            float x0 = dir > 0 ? front - 5.f : front - look;
            float x1 = dir > 0 ? front + look : front + 5.f;
            float ceilY = 0.f, floorY = 0.f;
            bool hasCeil = false, hasFloor = false;

            forEachNear(x0, x1, [&](Entry const& e, CCRect const& r) {
                if (e.kind == Kind::Ring) return;
                if (r.getMaxX() < x0 || r.getMinX() > x1) return;
                // "above" = the obstacle limits us from above (acts as a ceiling).
                // A straddling obstacle counts as ceiling if most of it is above us.
                bool above;
                if (r.getMinY() >= py) above = true;
                else if (r.getMaxY() <= py) above = false;
                else above = (r.getMaxY() - py) > (py - r.getMinY());
                if (above) {
                    ceilY = hasCeil ? std::min(ceilY, r.getMinY()) : r.getMinY();
                    hasCeil = true;
                } else {
                    floorY = hasFloor ? std::max(floorY, r.getMaxY()) : r.getMaxY();
                    hasFloor = true;
                }
            });

            float target = py;
            if (hasCeil && hasFloor) target = (ceilY + floorY) / 2.f;
            else if (hasCeil) target = ceilY - 45.f;
            else if (hasFloor) target = floorY + 45.f;

            if (p->m_isDart) {
                // Wave: holding moves "up" relative to gravity.
                return (target - py) * g > 2.f;
            }
            if (p->m_isSwing) {
                // Swing: each press flips gravity. Want gravity pointing toward target.
                if (f->m_holding) return false;
                bool wantUp = target > py + 4.f;
                bool wantDown = target < py - 4.f;
                bool gravityUp = p->m_isUpsideDown;
                return (wantUp && !gravityUp) || (wantDown && gravityUp);
            }
            float predicted = py + g * vel * 4.f;
            if (p->m_isBird) {
                // UFO: tap when falling below the target.
                if (f->m_holding) return false;
                return (target - predicted) * g > 0.f && vel <= 0.f;
            }
            // Ship
            return (target - predicted) * g > 0.f;
        }

        // ---- ground modes: jump/flip before obstacles ----
        float reach = 32.f * s;
        float zx0 = dir > 0 ? front - 2.f : front - reach;
        float zx1 = dir > 0 ? front + reach : front + 2.f;
        bool danger = false;
        bool ringTouch = false;

        forEachNear(zx0 - 30.f, zx1 + 30.f, [&](Entry const& e, CCRect const& r) {
            if (e.kind == Kind::Ring) {
                if (r.intersectsRect(pr)) ringTouch = true;
                return;
            }
            if (!overlaps(zx0, zx1, r.getMinX(), r.getMaxX())) return;
            if (e.kind == Kind::Hazard) {
                if (overlaps(pr.getMinY() - 4.f, pr.getMaxY() + 4.f, r.getMinY(), r.getMaxY()))
                    danger = true;
            } else if (e.kind == Kind::Solid) {
                // A wall we would run into (not a floor we can walk onto).
                bool wall = g > 0
                    ? (r.getMaxY() > pr.getMinY() + 10.f && r.getMinY() < pr.getMaxY() - 2.f)
                    : (r.getMinY() < pr.getMaxY() - 10.f && r.getMaxY() > pr.getMinY() + 2.f);
                if (wall) danger = true;
            }
        });

        bool ringWant = ringTouch && !p->m_isOnGround && vel <= 0.f;

        if (p->m_isBall || p->m_isSpider) {
            if (f->m_holding) return false;
            return (danger && p->m_isOnGround) || ringWant;
        }
        // Cube and robot: hold while something is ahead.
        if (ringWant && f->m_holding) return false;  // re-tap for the ring
        return danger || ringWant;
    }

    void runBot() {
        auto f = m_fields.self();
        bool on = isBotEnabled();
        if (f->m_botLabel) f->m_botLabel->setVisible(on);

        if (!on || m_isPlatformer || m_hasCompletedLevel || !m_player1 || m_player1->m_isDead) {
            if (f->m_holding) setHold(false);
            return;
        }
        buildEntries();
        if (!f->m_built) return;

        f->m_cheatedThisAttempt = true;
        setHold(decide(m_player1));
    }
};

// Run the bot once per physics tick, before queued inputs are processed.
class $modify(AutoBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            static_cast<AutoPlayLayer*>(pl)->runBot();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Pause menu toggles
// ---------------------------------------------------------------------------

class $modify(AutoPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("pause-button")) return;

        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("left-button-menu"));
        if (!menu) {
            // Fallback if node IDs are unavailable: our own small menu, top-left.
            menu = CCMenu::create();
            if (!menu) return;
            auto win = CCDirector::get()->getWinSize();
            menu->setPosition({35.f, win.height - 80.f});
            menu->setContentSize({40.f, 100.f});
            menu->setLayout(ColumnLayout::create());
            menu->setID("auto-complete-menu"_spr);
            this->addChild(menu, 10);
        }

        addToggle(menu, "Bot", menu_selector(AutoPauseLayer::onToggleBot),
            isBotEnabled(), "bot-toggle"_spr);
        addToggle(menu, "Noclip", menu_selector(AutoPauseLayer::onToggleNoclip),
            isNoclipEnabled(), "noclip-toggle"_spr);

        menu->updateLayout();
    }

    void addToggle(CCMenu* menu, char const* text, SEL_MenuHandler selector, bool value, std::string const& id) {
        auto toggler = CCMenuItemToggler::createWithStandardSprites(this, selector, 0.7f);
        if (!toggler) return;
        toggler->toggle(value);
        toggler->setID(id);

        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        if (label) {
            label->setScale(0.3f);
            label->setPosition({toggler->getContentWidth() / 2.f, -6.f});
            toggler->addChild(label);
        }
        menu->addChild(toggler);
    }

    static void markCheated() {
        if (auto pl = PlayLayer::get()) {
            static_cast<AutoPlayLayer*>(pl)->m_fields->m_cheatedThisAttempt = true;
        }
    }

    // CCMenuItemToggler fires before flipping its own state.
    void onToggleBot(CCObject* sender) {
        bool newValue = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("bot", newValue);
        if (newValue) markCheated();
    }

    void onToggleNoclip(CCObject* sender) {
        bool newValue = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("enabled", newValue);
        if (newValue) markCheated();
    }
};
