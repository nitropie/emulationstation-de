//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiProfile.cpp
//
//  The PROFILE main-menu screen.
//

#include "guis/GuiProfile.h"

#include "Window.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "profiles/ProfileMode.h"
#include "renderers/Renderer.h"

namespace
{
    constexpr int sColumns {4};
    constexpr float sTileSize {96.0f};
    constexpr float sGap {18.0f};
    constexpr float sGridTop {240.0f};
    constexpr unsigned int sBackgroundColor {0x14141EFF};
    constexpr unsigned int sTileColor {0x2A2A38FF};
    constexpr unsigned int sHighlightColor {0xF2C14EFF};
    constexpr unsigned int sTextColor {0xF5F5F5FF};
    constexpr unsigned int sDimTextColor {0x9A9AA8FF};
    constexpr unsigned int sErrorColor {0xFF8A80FF};
} // namespace

GuiProfile::GuiProfile()
{
    mWidth = Renderer::getInstance()->getScreenWidth();
    mHeight = Renderer::getInstance()->getScreenHeight();
    setPosition(0.0f, 0.0f);
    setSize(mWidth, mHeight);

    mReadOnly = !ProfileMode::isResolved();

    mTitle = std::make_shared<TextComponent>(_("PROFILE"), Font::get(FONT_SIZE_LARGE),
                                             sTextColor, ALIGN_CENTER);
    mTitle->setPosition(0.0f, 54.0f);
    mTitle->setSize(mWidth, 46.0f);
    addChild(mTitle.get());

    mName = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM), sTextColor,
                                            ALIGN_CENTER);
    const glm::vec2 namePosition {nameRowPosition()};
    mName->setPosition(namePosition.x, namePosition.y + 8.0f);
    mName->setSize(mWidth - 2.0f * namePosition.x, 30.0f);
    addChild(mName.get());

    mAvatarLabel = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM),
                                                   sTextColor, ALIGN_CENTER);
    mAvatarLabel->setPosition(0.0f, sGridTop + 4.0f * (sTileSize + sGap) + 12.0f);
    mAvatarLabel->setSize(mWidth, 30.0f);
    addChild(mAvatarLabel.get());

    mMessage = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM), sDimTextColor,
                                               ALIGN_CENTER);
    mMessage->setPosition(0.0f, mHeight - 92.0f);
    mMessage->setSize(mWidth, 30.0f);
    addChild(mMessage.get());

    mHint = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_SMALL), sDimTextColor,
                                            ALIGN_CENTER);
    mHint->setPosition(0.0f, mHeight - 58.0f);
    mHint->setSize(mWidth, 24.0f);
    addChild(mHint.get());

    rebuild();
    refresh();

    if (mReadOnly)
        setMessage(ProfileMode::error().empty() ? _("PROFILE IS NOT USABLE")
                                                : ProfileMode::error(),
                   true);
}

glm::vec2 GuiProfile::nameRowPosition() const
{
    constexpr float width {600.0f};
    return {(mWidth - width) / 2.0f, 150.0f};
}

glm::vec2 GuiProfile::tilePosition(const int index) const
{
    const int column {index % sColumns};
    const int row {index / sColumns};
    const float gridWidth {sColumns * sTileSize + (sColumns - 1) * sGap};
    const float x {(mWidth - gridWidth) / 2.0f + column * (sTileSize + sGap)};
    const float y {sGridTop + row * (sTileSize + sGap)};
    return {x, y};
}

int GuiProfile::avatarCount() const
{
    return static_cast<int>(mAvatarIds.size());
}

void GuiProfile::rebuild()
{
    for (const std::shared_ptr<ImageComponent>& tile : mTiles) {
        removeChild(tile.get());
    }
    mTiles.clear();
    mAvatarIds.clear();

    ProfileAvatars::Manifest* manifest {ProfileMode::manifest()};
    if (manifest == nullptr)
        return;

    for (const ProfileAvatars::AvatarEntry& entry : manifest->avatars()) {
        const glm::vec2 position {tilePosition(static_cast<int>(mTiles.size()))};
        std::shared_ptr<ImageComponent> tile {std::make_shared<ImageComponent>()};
        tile->setPosition(position.x, position.y);
        tile->setSize(sTileSize, sTileSize);
        tile->setImage(manifest->pathFor(entry));
        addChild(tile.get());
        mTiles.push_back(tile);
        mAvatarIds.push_back(entry.id);
    }
}

void GuiProfile::refresh()
{
    ProfileMetadata* metadata {ProfileMode::metadata()};
    ProfileAvatars::Manifest* manifest {ProfileMode::manifest()};

    if (metadata != nullptr) {
        mName->setText(std::string {_("NAME")} + "   " + metadata->displayName());
    }
    else {
        mName->setText(std::string {_("NAME")} + "   -");
    }

    if (metadata != nullptr && manifest != nullptr) {
        const ProfileAvatars::AvatarEntry& displayed {manifest->displayFor(metadata->avatarId())};
        mAvatarLabel->setText(displayed.label);
        for (std::size_t index {0}; index < mAvatarIds.size(); ++index) {
            const bool selected {mAvatarIds[index] == displayed.id};
            mTiles[index]->setOpacity(selected ? 1.0f : 0.65f);
        }
    }
    else {
        mAvatarLabel->setText("");
    }

    if (!mReadOnly) {
        mHint->setText(_("A: EDIT NAME OR PICK AVATAR     B: BACK"));
    }
    else {
        mHint->setText(_("READ ONLY"));
    }
}

void GuiProfile::setMessage(const std::string& text, const bool isError)
{
    mMessage->setColor(isError ? sErrorColor : sDimTextColor);
    mMessage->setText(text);
}

void GuiProfile::openNameEditor()
{
    ProfileMetadata* metadata {ProfileMode::metadata()};
    if (mReadOnly || metadata == nullptr)
        return;

    mWindow->pushGui(new GuiTextEditKeyboardPopup(
        0.5f, _("PROFILE NAME"), metadata->displayName(),
        [this](const std::string& value) { applyName(value); }, false));
}

void GuiProfile::applyName(const std::string& value)
{
    ProfileMetadata* metadata {ProfileMode::metadata()};
    if (metadata == nullptr)
        return;

    std::string error;
    if (!metadata->setDisplayName(value, error)) {
        setMessage(error, true);
        return;
    }

    if (!ProfileMode::save(error)) {
        setMessage(error, true);
        // Keep the screen consistent with the file after a failed write.
        ProfileMode::reloadMetadata();
        refresh();
        return;
    }

    refresh();
    setMessage(_("SAVED"), false);
}

void GuiProfile::selectAvatar(const int index)
{
    ProfileAvatars::Manifest* manifest {ProfileMode::manifest()};
    if (mReadOnly || manifest == nullptr || index < 0 || index >= avatarCount())
        return;

    ProfileMetadata* metadata {ProfileMode::metadata()};
    if (metadata == nullptr)
        return;

    std::string error;
    if (!metadata->setAvatar(mAvatarIds[index], error)) {
        setMessage(error, true);
        return;
    }

    if (!ProfileMode::save(error)) {
        setMessage(error, true);
        ProfileMode::reloadMetadata();
        refresh();
        return;
    }

    refresh();
    setMessage(_("SAVED"), false);
}

bool GuiProfile::input(InputConfig* config, Input input)
{
    if (input.value == 0)
        return false;

    if (config->isMappedTo("b", input)) {
        delete this;
        return true;
    }

    if (mReadOnly)
        return true;

    const int count {avatarCount()};

    if (config->isMappedTo("up", input)) {
        if (mFocus > 0 && mFocus <= sColumns)
            mFocus = 0;
        else if (mFocus > sColumns)
            mFocus -= sColumns;
    }
    else if (config->isMappedTo("down", input)) {
        if (mFocus == 0)
            mFocus = count > 0 ? 1 : 0;
        else if (mFocus + sColumns <= count)
            mFocus += sColumns;
    }
    else if (config->isMappedTo("left", input)) {
        if (mFocus > 1)
            mFocus -= 1;
    }
    else if (config->isMappedTo("right", input)) {
        if (mFocus >= 1 && mFocus < count)
            mFocus += 1;
    }
    else if (config->isMappedTo("a", input)) {
        if (mFocus == 0)
            openNameEditor();
        else
            selectAvatar(mFocus - 1);
    }
    else {
        return false;
    }

    return true;
}

void GuiProfile::render(const glm::mat4& parentTrans)
{
    Renderer* renderer {Renderer::getInstance()};
    const glm::mat4 trans {parentTrans * getTransform()};

    renderer->drawRect(0.0f, 0.0f, mWidth, mHeight, sBackgroundColor, sBackgroundColor);

    // Name row.
    const glm::vec2 namePosition {nameRowPosition()};
    renderer->drawRect(namePosition.x, namePosition.y, mWidth - 2.0f * namePosition.x, 44.0f,
                       0x22222EFF, 0x22222EFF);
    if (mFocus == 0 && !mReadOnly) {
        renderer->drawRect(namePosition.x, namePosition.y, mWidth - 2.0f * namePosition.x,
                           3.0f, sHighlightColor, sHighlightColor);
        renderer->drawRect(namePosition.x, namePosition.y + 41.0f,
                           mWidth - 2.0f * namePosition.x, 3.0f, sHighlightColor,
                           sHighlightColor);
    }

    // Avatar tiles: a placeholder behind every image, then a focus border.
    for (std::size_t index {0}; index < mTiles.size(); ++index) {
        const glm::vec2 position {tilePosition(static_cast<int>(index))};
        renderer->drawRect(position.x, position.y, sTileSize, sTileSize, sTileColor,
                           sTileColor);
        mTiles[index]->render(trans);
        if (mFocus == static_cast<int>(index) + 1 && !mReadOnly) {
            renderer->drawRect(position.x - 3.0f, position.y - 3.0f, sTileSize + 6.0f, 3.0f,
                               sHighlightColor, sHighlightColor);
            renderer->drawRect(position.x - 3.0f, position.y + sTileSize, sTileSize + 6.0f,
                               3.0f, sHighlightColor, sHighlightColor);
            renderer->drawRect(position.x - 3.0f, position.y, 3.0f, sTileSize,
                               sHighlightColor, sHighlightColor);
            renderer->drawRect(position.x + sTileSize, position.y, 3.0f, sTileSize,
                               sHighlightColor, sHighlightColor);
        }
    }

    GuiComponent::render(trans);
}

std::vector<HelpPrompt> GuiProfile::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts;
    if (mReadOnly) {
        prompts.emplace_back("b", _("back"));
        return prompts;
    }

    prompts.emplace_back("a", _("select"));
    prompts.emplace_back("b", _("back"));
    return prompts;
}
