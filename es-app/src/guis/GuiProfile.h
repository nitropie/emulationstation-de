//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiProfile.h
//
//  The PROFILE main-menu screen: display name editing with the on-screen keyboard and an avatar
//  grid. Profile mode only; without the profile environment the entry does not exist.
//

#ifndef ES_APP_GUIS_GUI_PROFILE_H
#define ES_APP_GUIS_GUI_PROFILE_H

#include "GuiComponent.h"
#include "components/ImageComponent.h"
#include "components/TextComponent.h"

#include <memory>
#include <string>
#include <vector>

class GuiProfile : public GuiComponent
{
public:
    GuiProfile();

    void render(const glm::mat4& parentTrans) override;
    bool input(InputConfig* config, Input input) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void rebuild();
    void refresh();
    void openNameEditor();
    void applyName(const std::string& value);
    void selectAvatar(const int index);
    void setMessage(const std::string& text, const bool isError);
    int avatarCount() const;
    float scale() const;
    float tileSize() const;
    float tileGap() const;
    float gridTop() const;
    glm::vec2 nameRowSize() const;
    glm::vec2 tilePosition(int index) const;
    glm::vec2 nameRowPosition() const;

    std::shared_ptr<TextComponent> mTitle;
    std::shared_ptr<TextComponent> mName;
    std::shared_ptr<TextComponent> mAvatarLabel;
    std::shared_ptr<TextComponent> mMessage;
    std::shared_ptr<TextComponent> mHint;
    std::vector<std::shared_ptr<ImageComponent>> mTiles;
    std::vector<std::string> mAvatarIds;
    int mFocus {0};
    bool mReadOnly {false};
    float mWidth {1280.0f};
    float mHeight {720.0f};
};

#endif // ES_APP_GUIS_GUI_PROFILE_H
