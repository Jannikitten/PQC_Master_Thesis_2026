#include "ConnectionView.h"
#include "GuiApp.h"
#include "DtlsClient.h" // ConnectionStatus enum
#include "misc/cpp/imgui_stdlib.h"

using Safira::Theme;
using Safira::U32ToVec4;

namespace Safira {

// =============================================================================
// Font helpers (same as the ones originally in ClientLayer's anon namespace)
// =============================================================================

namespace {

ImFont* BoldFont() {
    ImFont* f = ApplicationGUI::GetFont("Bold");
    return f ? f : ImGui::GetFont();
}

} // anon

// =============================================================================
// Public API
// =============================================================================

void ConnectionView::SetInitialFields(const std::string& username,
                                       const std::string& address) {
    if (!username.empty()) m_Username = username;
    if (!address.empty())  m_ServerIP = address;
    m_Initialized = true;
}

void ConnectionView::OpenCropModal(int srcW, int srcH, CropRect defaultCrop,
                                    ImTextureID previewTex) {
    m_CropSrcWidth   = srcW;
    m_CropSrcHeight  = srcH;
    m_CropRect       = defaultCrop;
    m_CropPreviewTex = previewTex;
    m_ShowCropModal  = true;
}

void ConnectionView::Render(ConnectionStatus clientStatus,
                             ConnectionStatus storeStatus,
                             const std::string& debugMessage,
                             ImTextureID avatarTexture) {
    RenderConnectionModal(clientStatus, storeStatus, debugMessage, avatarTexture);
    RenderCropModal();
}

// =============================================================================
// Connection Modal
// =============================================================================

void ConnectionView::RenderConnectionModal(
    ConnectionStatus clientStatus,
    ConnectionStatus storeStatus,
    const std::string& debugMessage,
    ImTextureID avatarTexture)
{
    if (!m_ConnectionModalOpen &&
        clientStatus != ConnectionStatus::Connected &&
        !m_ShowCropModal &&
        !ImGui::IsPopupOpen("Crop Avatar##CropModal"))
        ImGui::OpenPopup("Connection");

    const auto& t = Theme::Get();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       t.BgPopup);
    ImGui::PushStyleColor(ImGuiCol_Border,         t.ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  t.BgPanel);
    ImGui::PushStyleColor(ImGuiCol_Text,           t.TextPrimary);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        t.BgFrame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, t.BgFrameHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  t.BgFrameActive);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

    const ImVec2 vpSize = ImGui::GetMainViewport()->WorkSize;
    const float modalW = std::clamp(vpSize.x * 0.52f, 500.0f, 640.0f);
    const float modalH = std::clamp(vpSize.y * 0.46f, 310.0f, 380.0f);
    ImGui::SetNextWindowSize({ modalW, modalH }, ImGuiCond_Always);
    m_ConnectionModalOpen = ImGui::BeginPopupModal(
        "Connection", nullptr, ImGuiWindowFlags_NoResize);

    if (!m_ConnectionModalOpen) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(8);
        return;
    }

    float statusReserve = 0.0f;
    if (clientStatus == ConnectionStatus::FailedToConnect) {
        statusReserve = debugMessage.empty() ? 24.0f : 42.0f;
    } else if (clientStatus == ConnectionStatus::Connecting) {
        statusReserve = 24.0f;
    }
    float bodyH = ImGui::GetContentRegionAvail().y - statusReserve - 6.0f;
    if (bodyH < 210.0f) bodyH = 210.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgPopupAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("##ConnectBody", { 0.0f, bodyH }, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float bodyW = ImGui::GetContentRegionAvail().x;
        const float leftW = std::clamp(bodyW * 0.34f, 160.0f, 220.0f);
        const float panelGap = 12.0f;
        const float panelH = ImGui::GetContentRegionAvail().y;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, t.BgFrame);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        // -- Left panel: avatar -----------------------------------------------
        ImGui::BeginChild("##ConnectAvatarPanel", { leftW, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImDrawList* panelDl = ImGui::GetWindowDrawList();
            const float avatarDiameter = 106.0f;
            const float avatarRadius = avatarDiameter * 0.5f;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float avatarIndent =
                std::max(0.0f, (availW - avatarDiameter) * 0.5f);

            ImGui::Dummy({ 0.0f, 6.0f });
            ImGui::Indent(avatarIndent);
            ImGui::InvisibleButton("##AvatarPreview",
                                   { avatarDiameter, avatarDiameter });
            ImGui::Unindent(avatarIndent);

            const ImVec2 avatarMin = ImGui::GetItemRectMin();
            const ImVec2 avatarMax = ImGui::GetItemRectMax();
            const ImVec2 center = {
                (avatarMin.x + avatarMax.x) * 0.5f,
                (avatarMin.y + avatarMax.y) * 0.5f
            };

            panelDl->AddCircleFilled(center, avatarRadius + 8.0f,
                                     IM_COL32(44, 46, 62, 255), 48);
            panelDl->AddCircle(center, avatarRadius + 8.0f,
                               t.InputBorder, 48, 1.2f);

            if (avatarTexture) {
                panelDl->AddImageRounded(avatarTexture,
                    { center.x - avatarRadius, center.y - avatarRadius },
                    { center.x + avatarRadius, center.y + avatarRadius },
                    { 0, 0 }, { 1, 1 },
                    t.AvatarImageTint, avatarRadius);
            } else {
                panelDl->AddCircleFilled(center, avatarRadius,
                                         IM_COL32(84, 84, 96, 255), 48);
                char letter = m_Username.empty() ? '?'
                    : static_cast<char>(toupper(m_Username[0]));
                char buf[2] = { letter, '\0' };
                ImVec2 lsz = ImGui::CalcTextSize(buf);
                panelDl->AddText(
                    { center.x - lsz.x * 0.5f, center.y - lsz.y * 0.5f },
                    IM_COL32(236, 236, 242, 255), buf);
            }

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            float browseW = ImGui::GetContentRegionAvail().x;
            if (avatarTexture)
                browseW = std::max(96.0f, browseW - 66.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10.0f, 7.0f });
            if (ImGui::Button("Browse Image", { browseW, actionH })) {
                if (m_OnBrowse) {
                    auto path = m_OnBrowse();
                    if (path && m_ShowCropModal)
                        ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            if (avatarTexture) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.LogoutBtnHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.LogoutBtnActive);
                ImGui::PushStyleColor(ImGuiCol_Text, t.LogoutIcon);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 8.0f, 7.0f });
                if (ImGui::Button("Clear", { 60.0f, actionH })) {
                    if (m_OnAvatarCleared)
                        m_OnAvatarCleared();
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(4);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, panelGap);

        // -- Right panel: connection form -------------------------------------
        ImGui::BeginChild("##ConnectFormPanel", { 0.0f, panelH }, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Username");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##username", &m_Username);

            ImGui::Dummy({ 0.0f, 10.0f });
            ImGui::TextColored(U32ToVec4(t.TextSecondary), "Server Address");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##address", &m_ServerIP);

            const float actionH = 36.0f;
            const float footerY = ImGui::GetWindowHeight()
                - ImGui::GetStyle().WindowPadding.y - actionH;
            if (ImGui::GetCursorPosY() < footerY)
                ImGui::SetCursorPosY(footerY);

            const float actionW =
                (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

            ImGui::PushStyleColor(ImGuiCol_Button,        t.SendBtn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.SendBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  t.SendBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text,          t.SendBtnText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Connect", { actionW, actionH })) {
                if (m_OnConnect) {
                    std::string addr = m_ServerIP;
                    if (addr.rfind(':') == std::string::npos)
                        addr += ":8192";
                    m_OnConnect(addr, m_Username);
                }
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0.0f, 12.0f);

            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.LogoutBtnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.LogoutBtnActive);
            ImGui::PushStyleColor(ImGuiCol_Text, t.LogoutIcon);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 12.0f, 8.0f });
            if (ImGui::Button("Quit", { actionW, actionH })) {
                if (m_OnQuit) m_OnQuit();
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(1);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    // -- Connection status feedback -------------------------------------------
    if (clientStatus == ConnectionStatus::Connected
        && storeStatus == ConnectionStatus::Connected)
    {
        ImGui::CloseCurrentPopup();
    } else if (clientStatus == ConnectionStatus::FailedToConnect) {
        ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "Connection failed.");
        if (!debugMessage.empty())
            ImGui::TextColored({ 0.9f, 0.2f, 0.1f, 1.0f }, "%s",
                               debugMessage.c_str());
    } else if (clientStatus == ConnectionStatus::Connecting) {
        ImGui::TextColored({ 0.8f, 0.8f, 0.8f, 1.0f }, "Connecting...");
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);
}

// =============================================================================
// Crop Modal
// =============================================================================

void ConnectionView::RenderCropModal() {
    if (m_ShowCropModal) {
        ImGui::OpenPopup("Crop Avatar##CropModal");
        m_ShowCropModal = false;
    }

    const auto& t = Theme::Get();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, t.BgPopupAlt);
    ImGui::PushStyleColor(ImGuiCol_Border,   t.ModalBorder);
    ImGui::PushStyleColor(ImGuiCol_Text,     t.TextPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

    bool open = true;
    if (ImGui::BeginPopupModal("Crop Avatar##CropModal", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {

        ImFont* bold = BoldFont();
        if (bold) ImGui::PushFont(bold);
        ImGui::Text("Crop to Square");
        if (bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        m_CropRect = DrawCropWidget(m_CropPreviewTex,
                                     m_CropSrcWidth, m_CropSrcHeight,
                                     m_CropRect, 300.0f);

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        t.Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   t.AccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text,           t.AccentText);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Apply Crop", { 120, 0 })) {
            if (m_OnCropApplied)
                m_OnCropApplied(m_CropImagePath, m_CropRect);
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,        t.DeclineBtn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  t.DeclineBtnHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

        if (ImGui::Button("Cancel", { 80, 0 })) {
            if (m_OnAvatarCleared)
                m_OnAvatarCleared();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::EndPopup();
    }

    if (!open) {
        if (m_OnAvatarCleared)
            m_OnAvatarCleared();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

} // namespace Safira
