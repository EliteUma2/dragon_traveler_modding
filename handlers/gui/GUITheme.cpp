#include "pch-il2cpp.h"
#include "gui/GUITheme.h"
#include <imgui/imgui.h>

void ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    const ImVec4 BgColor = ImVec4(0.22f, 0.22f, 0.22f, 0.40f);
    const ImVec4 ChildBg = ImVec4(0.20f, 0.20f, 0.20f, 0.40f);
    const ImVec4 Border = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    const ImVec4 Text = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    const ImVec4 TextDis = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    const ImVec4 UnityBlue = ImVec4(0.17f, 0.36f, 0.53f, 1.00f);
    const ImVec4 ElementHover = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    const ImVec4 ElementActive = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);

    colors[ImGuiCol_Text] = Text;
    colors[ImGuiCol_TextDisabled] = TextDis;
    colors[ImGuiCol_WindowBg] = BgColor;
    colors[ImGuiCol_ChildBg] = ChildBg;
    colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

    colors[ImGuiCol_Border] = Border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ElementActive;

    colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    colors[ImGuiCol_CheckMark] = Text;
    colors[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ElementHover;
    colors[ImGuiCol_HeaderActive] = UnityBlue;

    colors[ImGuiCol_Separator] = Border;
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ElementHover;
    colors[ImGuiCol_ResizeGripActive] = UnityBlue;

    colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered] = ElementHover;
    colors[ImGuiCol_TabActive] = UnityBlue;
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    colors[ImGuiCol_TextSelectedBg] = UnityBlue;

    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;

    style.WindowRounding = 4.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    style.TouchExtraPadding = ImVec2(2.0f, 2.0f);
}