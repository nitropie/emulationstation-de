//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  UiTestDriver.cpp
//
//  Native UI test driver. See UiTestDriver.h and es-app/tests/ui/README.md.
//

#include "UiTestDriver.h"

#include "GuiComponent.h"
#include "UiTestScript.h"
#include "Window.h"
#include "renderers/Renderer.h"
#include "views/ViewController.h"

#if defined(USE_OPENGLES)
#include <GLES3/gl3.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include <SDL2/SDL_events.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    bool sEnabled {false};
    bool sFailed {false};
    bool sFinished {false};
    // The scenario expects the application to exit on its own, for example through a dialog that
    // quits. The runner accepts a clean exit instead of a PASS line in that case.
    bool sExitExpected {false};
    std::vector<UiTest::Step> sSteps;
    std::size_t sIndex {0};
    int sFrame {0};

    SDL_GameController* sController {nullptr};
    SDL_JoystickID sControllerId {-1};

    std::vector<std::string> sSceneText;
    int sSceneTextFrame {-1};

    std::uint64_t fnv1a(const unsigned char* data, const std::size_t size)
    {
        std::uint64_t hash {1469598103934665603ULL};
        for (std::size_t i {0}; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }

        return hash;
    }

    void collectText(GuiComponent* component,
                     const bool parentVisible,
                     const glm::vec2 parentPosition,
                     std::vector<std::string>& out,
                     const int depth)
    {
        if (component == nullptr || depth > 40)
            return;

        const bool visible {parentVisible && component->isVisible()};
        if (!visible)
            return;

        const glm::vec3 position {component->getPosition()};
        const glm::vec2 absolute {parentPosition.x + position.x, parentPosition.y + position.y};
        const std::string value {component->getValue()};
        if (!value.empty()) {
            const glm::vec2 size {component->getSize()};
            out.emplace_back("[" + std::to_string(static_cast<int>(absolute.x)) + "," +
                             std::to_string(static_cast<int>(absolute.y)) + " " +
                             std::to_string(static_cast<int>(size.x)) + "x" +
                             std::to_string(static_cast<int>(size.y)) + "] " + value);
        }

        for (unsigned int i {0}; i < component->getChildCount(); ++i)
            collectText(component->getChild(i), visible, absolute, out, depth + 1);
    }

    const std::vector<std::string>& sceneText()
    {
        if (sSceneTextFrame != sFrame) {
            sSceneText.clear();
            collectText(Window::getInstance()->peekGui(), true, {0.0f, 0.0f}, sSceneText, 0);
            std::sort(sSceneText.begin(), sSceneText.end());
            sSceneTextFrame = sFrame;
        }

        return sSceneText;
    }

    bool sceneHasView(const std::string& view)
    {
        const ViewController::ViewMode mode {ViewController::getInstance()->getState().viewing};

        if (view == "gamelist")
            return mode == ViewController::ViewMode::GAMELIST;

        return mode == ViewController::ViewMode::SYSTEM_SELECT;
    }

    bool sceneHasText(const std::string& needle)
    {
        for (const std::string& line : sceneText()) {
            if (line.find(needle) != std::string::npos)
                return true;
        }

        return false;
    }

    void printScene(const std::string& name)
    {
        const int width {static_cast<int>(Renderer::getScreenWidth())};
        const int height {static_cast<int>(Renderer::getScreenHeight())};
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        const std::uint64_t full {fnv1a(pixels.data(), pixels.size())};
        const std::uint64_t top {
            fnv1a(pixels.data(), static_cast<std::size_t>(width) * (height / 2) * 4)};
        const std::uint64_t bottom {
            fnv1a(pixels.data() + static_cast<std::size_t>(width) * (height / 2) * 4,
                  static_cast<std::size_t>(width) * (height - height / 2) * 4)};

        std::printf("[ui-test] capture %s frame %d %dx%d full=%016llx top=%016llx bottom=%016llx\n",
                    name.c_str(), sFrame, width, height, static_cast<unsigned long long>(full),
                    static_cast<unsigned long long>(top), static_cast<unsigned long long>(bottom));

        const std::vector<std::string>& text {sceneText()};
        std::printf("[ui-test] capture %s text-count %zu\n", name.c_str(), text.size());
        for (const std::string& line : text)
            std::printf("[ui-test] capture %s text %s\n", name.c_str(), line.c_str());
    }

    bool pushKey(const std::string& name, const bool down)
    {
        SDL_Keycode key {SDLK_UNKNOWN};

        if (name == "escape")
            key = SDLK_ESCAPE;
        else if (name == "return" || name == "enter")
            key = SDLK_RETURN;
        else if (name == "backspace")
            key = SDLK_BACKSPACE;
        else if (name == "delete")
            key = SDLK_DELETE;
        else if (name == "insert")
            key = SDLK_INSERT;
        else if (name == "up")
            key = SDLK_UP;
        else if (name == "down")
            key = SDLK_DOWN;
        else if (name == "left")
            key = SDLK_LEFT;
        else if (name == "right")
            key = SDLK_RIGHT;
        else if (name == "pageup")
            key = SDLK_PAGEUP;
        else if (name == "pagedown")
            key = SDLK_PAGEDOWN;
        else if (name == "home")
            key = SDLK_HOME;
        else if (name == "end")
            key = SDLK_END;
        else
            key = SDL_GetKeyFromName(name.c_str());

        if (key == SDLK_UNKNOWN) {
            std::printf("[ui-test] FAIL unknown key \"%s\"\n", name.c_str());
            return false;
        }

        SDL_Event event;
        std::memset(&event, 0, sizeof(event));
        event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        event.key.repeat = 0;
        event.key.keysym.sym = key;
        event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
        SDL_PushEvent(&event);

        return true;
    }

    bool controllerButton(const std::string& name, Uint8& button)
    {
        if (name == "a")
            button = SDL_CONTROLLER_BUTTON_A;
        else if (name == "b")
            button = SDL_CONTROLLER_BUTTON_B;
        else if (name == "x")
            button = SDL_CONTROLLER_BUTTON_X;
        else if (name == "y")
            button = SDL_CONTROLLER_BUTTON_Y;
        else if (name == "start")
            button = SDL_CONTROLLER_BUTTON_START;
        else if (name == "back")
            button = SDL_CONTROLLER_BUTTON_BACK;
        else if (name == "up")
            button = SDL_CONTROLLER_BUTTON_DPAD_UP;
        else if (name == "down")
            button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        else if (name == "left")
            button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        else if (name == "right")
            button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        else if (name == "leftshoulder")
            button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
        else if (name == "rightshoulder")
            button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        else
            return false;

        return true;
    }

    bool attachController()
    {
        if (sController != nullptr)
            return true;

        const int index {SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0,
                                                   SDL_CONTROLLER_BUTTON_MAX, 0)};
        if (index < 0) {
            std::printf("[ui-test] FAIL could not attach a virtual controller: %s\n",
                        SDL_GetError());
            return false;
        }

        sController = SDL_GameControllerOpen(index);
        if (sController == nullptr) {
            std::printf("[ui-test] FAIL could not open the virtual controller: %s\n",
                        SDL_GetError());
            return false;
        }

        sControllerId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sController));
        std::printf("[ui-test] controller attached instance=%d\n", static_cast<int>(sControllerId));

        return true;
    }

    bool pushController(const std::string& name, const bool down)
    {
        Uint8 button {0};
        if (!controllerButton(name, button)) {
            std::printf("[ui-test] FAIL unknown controller button \"%s\"\n", name.c_str());
            return false;
        }

        if (!attachController())
            return false;

        SDL_Event event;
        std::memset(&event, 0, sizeof(event));
        event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.which = sControllerId;
        event.cbutton.button = button;
        event.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
        SDL_PushEvent(&event);

        return true;
    }

    void pushText(const std::string& text)
    {
        // A pushed SDL2 text event segfaults inside SDL_PushEvent: sdl2-compat bridges the
        // SDL2 event, whose text lives in an inline fixed-size buffer, to the SDL3 text event,
        // whose string has different ownership, and SDL3 dereferences the wrong pointer. Key and
        // controller events carry no pointer payload and are unaffected.
        //
        // Deliver the text through the same entry point main() uses for SDL_TEXTINPUT, so the
        // text still travels the application's real text-input path without the broken bridge.
        Window* window {Window::getInstance()};
        if (window != nullptr) {
            // The text component masks injected text while a key is being processed; real typing
            // works because SDL sends KEYDOWN (which unmasks) just before TEXTINPUT. A scenario
            // has no such key, so it takes the paste path, which the component accepts while
            // masked and which inserts the text the same way.
            window->textInput(text, true);
        }
    }

    void pushQuit()
    {
        SDL_Event event;
        std::memset(&event, 0, sizeof(event));
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
    }

    void fail(const UiTest::Step& step, const std::string& message)
    {
        sFailed = true;
        std::printf("[ui-test] FAIL line %d: %s\n", step.line, message.c_str());
        for (const std::string& line : sceneText())
            std::printf("[ui-test] scene %s\n", line.c_str());
        pushQuit();
    }

} // namespace

void UiTest::initialize()
{
    const char* scriptPath {getenv("ES_DE_UI_TEST_SCRIPT")};
    if (scriptPath == nullptr || *scriptPath == '\0')
        return;

    sEnabled = true;
    std::printf("[ui-test] WARNING: the UI test driver is active, script %s. "
                "The frontend is being driven by injected SDL events.\n",
                scriptPath);

    std::ifstream file {scriptPath};
    if (!file) {
        std::printf("[ui-test] FAIL could not read the script %s\n", scriptPath);
        sFailed = true;
        pushQuit();
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    std::string error;
    if (!parseScript(buffer.str(), sSteps, error)) {
        std::printf("[ui-test] FAIL invalid script: %s\n", error.c_str());
        sFailed = true;
        pushQuit();
    }
}

void UiTest::frame()
{
    if (!sEnabled || sFailed || sFinished)
        return;

    ++sFrame;

    while (sIndex < sSteps.size()) {
        Step& step {sSteps[sIndex]};

        switch (step.verb) {
            case Verb::waitFrames: {
                if (step.frames > 0) {
                    --step.frames;
                    return;
                }
                ++sIndex;
                break;
            }
            case Verb::waitView: {
                if (sceneHasView(step.text)) {
                    ++sIndex;
                    break;
                }
                if (--step.frames <= 0) {
                    fail(step, "timed out waiting for the " + step.text + " view");
                    return;
                }
                return;
            }
            case Verb::waitText: {
                if (sceneHasText(step.text)) {
                    ++sIndex;
                    break;
                }
                if (--step.frames <= 0) {
                    fail(step, "timed out waiting for \"" + step.text + "\"");
                    return;
                }
                return;
            }
            case Verb::assertText: {
                if (!sceneHasText(step.text)) {
                    fail(step, "the scene does not show \"" + step.text + "\"");
                    return;
                }
                ++sIndex;
                break;
            }
            case Verb::assertAbsent: {
                if (sceneHasText(step.text)) {
                    fail(step, "the scene still shows \"" + step.text + "\"");
                    return;
                }
                ++sIndex;
                break;
            }
            case Verb::capture: {
                printScene(step.text);
                ++sIndex;
                break;
            }
            case Verb::key: {
                if (!pushKey(step.name, step.pressed)) {
                    fail(step, "could not push the key \"" + step.name + "\"");
                    return;
                }
                std::printf("[ui-test] key %s %s\n", step.name.c_str(),
                            step.pressed ? "down" : "up");
                ++sIndex;
                break;
            }
            case Verb::controller: {
                if (!pushController(step.name, step.pressed)) {
                    fail(step, "could not push the controller button \"" + step.name + "\"");
                    return;
                }
                std::printf("[ui-test] controller %s %s\n", step.name.c_str(),
                            step.pressed ? "down" : "up");
                ++sIndex;
                break;
            }
            case Verb::text: {
                pushText(step.text);
                std::printf("[ui-test] text %s\n", step.text.c_str());
                ++sIndex;
                break;
            }
            case Verb::expectExit: {
                std::printf("[ui-test] expect-exit\n");
                sExitExpected = true;
                ++sIndex;
                break;
            }
            case Verb::quit: {
                std::printf("[ui-test] PASS %zu steps\n", sSteps.size());
                sFinished = true;
                ++sIndex;
                pushQuit();
                return;
            }
        }
    }

    if (!sFinished && sIndex >= sSteps.size() && !sExitExpected) {
        const Step last {sSteps.back()};
        fail(last, "the script ended without quit");
    }
}
