// Chat window opened by the movable chat button in the editor.

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include "Collab.hpp"

using namespace geode::prelude;

namespace collab {

static constexpr size_t kLineChars = 48;   // characters per visual line
static constexpr int kVisibleLines = 9;

class ChatPopup : public Popup {
protected:
    TextInput* m_input = nullptr;
    CCNode* m_lines = nullptr;
    size_t m_shownCount = static_cast<size_t>(-1);

    bool init() {
        if (!Popup::init(340.f, 240.f)) return false;
        this->setTitle(fmt::format("Chat - room {}", Session::get().room()));
        this->setID("live-collab-chat"_spr);

        auto bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(90);
        bg->setContentSize({ 310.f, 135.f });
        m_mainLayer->addChildAtPosition(bg, Anchor::Center, ccp(0, 12));

        m_lines = CCNode::create();
        m_lines->setContentSize({ 300.f, 128.f });
        m_lines->setAnchorPoint({ .5f, .5f });
        m_mainLayer->addChildAtPosition(m_lines, Anchor::Center, ccp(0, 12));

        m_input = TextInput::create(230.f, "Type a message...", "chatFont.fnt");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(120);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Bottom, ccp(-35, 28));

        auto sendBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Send", "goldFont.fnt", "GJ_button_01.png", .6f),
            this, menu_selector(ChatPopup::onSend)
        );
        m_buttonMenu->addChildAtPosition(sendBtn, Anchor::Bottom, ccp(125, 28));

        G().chatOpen = true;
        G().unread = 0;
        this->refresh();
        this->schedule(schedule_selector(ChatPopup::checkNew), .25f);
        return true;
    }

    // Splits a message into lines that fit the window
    static std::vector<std::string> wrap(std::string const& text) {
        std::vector<std::string> out;
        std::string line;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t space = text.find(' ', pos);
            std::string word = text.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
            pos = space == std::string::npos ? text.size() : space + 1;
            while (word.size() > kLineChars) {
                if (!line.empty()) { out.push_back(line); line.clear(); }
                out.push_back(word.substr(0, kLineChars));
                word = word.substr(kLineChars);
            }
            if (line.size() + word.size() + (line.empty() ? 0 : 1) > kLineChars) {
                out.push_back(line);
                line.clear();
            }
            if (!line.empty()) line += ' ';
            line += word;
        }
        if (!line.empty()) out.push_back(line);
        return out;
    }

    void refresh() {
        auto& chat = G().chat;
        m_shownCount = chat.size();
        m_lines->removeAllChildren();

        struct Visual { std::string text; ccColor3B color; };
        std::vector<Visual> visual;
        if (chat.empty()) {
            visual.push_back({ "No messages yet. Say hi!", { 160, 160, 160 } });
        }
        for (auto const& line : chat) {
            auto parts = wrap(line.name + ": " + line.text);
            ccColor3B color = line.mine ? ccColor3B { 140, 230, 255 } : ccColor3B { 255, 255, 255 };
            for (auto& p : parts) visual.push_back({ p, color });
        }
        // Only the newest lines fit
        size_t first = visual.size() > kVisibleLines ? visual.size() - kVisibleLines : 0;
        float y = m_lines->getContentSize().height - 7.f;
        for (size_t i = first; i < visual.size(); i++) {
            auto label = CCLabelBMFont::create(visual[i].text.c_str(), "chatFont.fnt");
            label->setScale(.55f);
            label->setAnchorPoint({ 0.f, .5f });
            label->setColor(visual[i].color);
            label->setPosition({ 4.f, y });
            m_lines->addChild(label);
            y -= 14.f;
        }
    }

    void checkNew(float) {
        G().unread = 0;
        if (G().chat.size() != m_shownCount) this->refresh();
    }

    void onSend(CCObject*) {
        std::string text = m_input->getString();
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text.empty()) return;
        auto& s = Session::get();
        if (s.state() != State::Connected) {
            addChatLine("System", "Not connected - message not sent", false);
        } else {
            s.send('C', myName() + "\n" + text);
            addChatLine(myName(), text, true);
        }
        m_input->setString("");
        this->refresh();
    }

public:
    void onClose(CCObject* sender) override {
        G().chatOpen = false;
        Popup::onClose(sender);
    }

    static ChatPopup* create() {
        auto ret = new ChatPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

bool chatIsShowing() {
    auto scene = CCDirector::get()->getRunningScene();
    bool showing = scene && scene->getChildByID("live-collab-chat"_spr);
    if (!showing) G().chatOpen = false;
    return showing;
}

void openChat() {
    if (chatIsShowing()) return;
    if (auto popup = ChatPopup::create()) popup->show();
}

} // namespace collab
