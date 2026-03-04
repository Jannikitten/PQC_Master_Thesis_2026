#ifndef PQC_MASTER_THESIS_2026_THEME_H
#define PQC_MASTER_THESIS_2026_THEME_H

// =============================================================================
// Theme.h -- Centralised colour palette for the Safira client
//
// Every visual colour used across ApplicationGUI, ClientLayer and ChatPanel
// is defined here.  Two built-in palettes are provided (Dark / Light) and
// the active palette can be switched at runtime with Theme::Toggle().
//
// Usage:
//     const auto& t = Safira::Theme::Get();
//     dl->AddRectFilled(min, max, t.BgPanel);
// =============================================================================

#include <imgui.h>
#include <cstdint>

namespace Safira {

// Helper: IM_COL32 → ImVec4
inline ImVec4 U32ToVec4(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

// ─────────────────────────────────────────────────────────────────────────────
// ThemeData — every semantic colour role used by the application
// ─────────────────────────────────────────────────────────────────────────────

struct ThemeData {

    // ── Backgrounds ──────────────────────────────────────────────────────
    ImU32 BgWindow;              // Vulkan clear colour / root window
    ImU32 BgTitlebar;            // Custom titlebar fill
    ImU32 BgPanel;               // Sidebar & chat‑area child panels
    ImU32 BgItemSelected;        // Sidebar conversation – selected
    ImU32 BgItemHovered;         // Sidebar conversation – hovered
    ImU32 BgOwnBubble;           // Own chat bubble
    ImU32 BgPeerBubble;          // Peer chat bubble
    ImU32 BgInput;               // Chat input field fill
    ImU32 BgPopup;               // Modal / popup background
    ImU32 BgPopupAlt;            // Secondary popup bg (invite, report)
    ImU32 BgFrame;               // Generic ImGui frame bg
    ImU32 BgFrameHovered;        // Frame bg – hover
    ImU32 BgFrameActive;         // Frame bg – active/focused

    // ── Borders & Lines ──────────────────────────────────────────────────
    ImU32 Separator;             // Panel separator lines (H + V)
    ImU32 Divider;               // Content dividers (online / convo list)
    ImU32 InputBorder;           // Chat input ring
    ImU32 InputShadow;           // Chat input outer shadow
    ImU32 ModalBorder;           // Modal window border
    ImU32 WindowBorder;          // Main window outer border
    ImU32 IconOutline;           // User icon circle outline

    // ── Accent ───────────────────────────────────────────────────────────
    ImU32 Accent;                // Primary accent (gold)
    ImU32 AccentHover;           // Accent – hover
    ImU32 AccentActive;          // Accent – pressed
    ImU32 AccentText;            // Text on accent background
    ImU32 AccentFaded;           // Decorative faded accent (logo)
    ImU32 AccentRing;            // Decorative ring stroke (logo)
    ImU32 AccentRingInner;       // Inner decorative ring
    ImU32 LobbyAvatar;           // Lobby conversation avatar bg

    // ── Text ─────────────────────────────────────────────────────────────
    ImU32 TextPrimary;           // Main readable text
    ImU32 TextSecondary;         // Less prominent text
    ImU32 TextMuted;             // Very subtle text / timestamps
    ImU32 TextSystem;            // System messages in chat
    ImU32 TextTitlebar;          // Username in titlebar
    ImU32 TextTagline;           // "Post-Quantum Secure Messaging"

    // ── Status ───────────────────────────────────────────────────────────
    ImU32 StatusOnline;
    ImU32 StatusAway;
    ImU32 StatusPending;         // Handshaking / pending
    ImU32 StatusOffline;
    ImU32 StatusDotInactive;     // Grey dot when disconnected

    // ── Buttons — ghost (titlebar) ───────────────────────────────────────
    ImU32 GhostBtnBg;
    ImU32 GhostBtnHover;
    ImU32 GhostBtnActive;

    // ── Buttons — accent (connect, accept, send, new‑convo) ─────────────
    // (uses Accent / AccentHover / AccentActive / AccentText directly)

    // ── Buttons — danger (leave private chat) ────────────────────────────
    ImU32 DangerBtn;
    ImU32 DangerBtnHover;
    ImU32 DangerBtnActive;
    ImU32 DangerBtnText;         // Text on danger button

    // ── Buttons — send ───────────────────────────────────────────────────
    ImU32 SendBtn;
    ImU32 SendBtnHover;
    ImU32 SendBtnActive;
    ImU32 SendBtnText;
    ImU32 SendBtnMuted;
    ImU32 SendBtnMutedHover;

    // ── Buttons — decline / cancel ───────────────────────────────────────
    ImU32 DeclineBtn;
    ImU32 DeclineBtnHover;
    ImU32 DeclineBtnActive;

    // ── Buttons — logout ─────────────────────────────────────────────────
    ImU32 LogoutBtnHover;
    ImU32 LogoutBtnActive;
    ImU32 LogoutIcon;
    ImU32 LogoutIconHover;

    // ── Scrollbar ────────────────────────────────────────────────────────
    ImU32 ScrollBg;
    ImU32 ScrollGrab;
    ImU32 ScrollGrabHover;
    ImU32 ScrollGrabActive;

    // ── Toggle icon (chat panel) ─────────────────────────────────────────
    ImU32 ToggleIconActive;
    ImU32 ToggleIconInactive;

    // ── Misc ─────────────────────────────────────────────────────────────
    ImU32 UnreadDot;             // Sidebar unread indicator
    ImU32 AvatarImageTint;       // Image avatar tint (usually white)
    ImU32 AvatarLetterCol;       // Letter inside fallback avatar
    ImU32 ConvoTitleCol;         // Conversation list title
    ImU32 ConvoPreviewCol;       // Conversation list preview
    ImU32 ConvoTimeCol;          // Conversation list time label
    ImU32 BubbleOutline;         // System bubble outline stroke

    // ── ImGui style overrides (applied via ApplyImGuiStyle) ──────────────
    ImU32 ImGuiWindowBg;
    ImU32 ImGuiChildBg;
    ImU32 ImGuiPopupBg;
    ImU32 ImGuiBorder;
    ImU32 ImGuiText;

    // ── Convenience: clear colour as ImVec4 ──────────────────────────────
    ImVec4 ClearColor() const {
        return U32ToVec4(BgWindow);
    }

    ImVec4 PanelBgVec4() const {
        return U32ToVec4(BgPanel);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Built-in palettes
// ─────────────────────────────────────────────────────────────────────────────

inline ThemeData DarkPalette() {
    ThemeData t{};

    // Backgrounds
    t.BgWindow          = IM_COL32( 36,  36,  36, 255);
    t.BgTitlebar        = IM_COL32( 28,  28,  28, 255);
    t.BgPanel           = IM_COL32( 36,  36,  36, 255);
    t.BgItemSelected    = IM_COL32( 28,  28,  28, 255);
    t.BgItemHovered     = IM_COL32( 32,  32,  32, 255);
    t.BgOwnBubble       = IM_COL32( 50,  64,  82, 255);
    t.BgPeerBubble      = IM_COL32( 50,  50,  50, 255);
    t.BgInput           = IM_COL32( 48,  48,  48, 255);
    t.BgPopup           = IM_COL32( 28,  28,  28, 245);
    t.BgPopupAlt        = IM_COL32( 32,  32,  32, 245);
    t.BgFrame           = IM_COL32( 48,  48,  48, 255);
    t.BgFrameHovered    = IM_COL32( 58,  58,  58, 255);
    t.BgFrameActive     = IM_COL32( 68,  68,  68, 255);

    // Borders & lines
    t.Separator         = IM_COL32( 48,  48,  48, 255);
    t.Divider           = IM_COL32( 48,  48,  48, 255);
    t.InputBorder       = IM_COL32( 68,  68,  68, 255);
    t.InputShadow       = IM_COL32(  0,   0,   0,  40);
    t.ModalBorder       = IM_COL32( 60,  60,  60, 200);
    t.WindowBorder      = IM_COL32( 50,  50,  50, 255);
    t.IconOutline       = IM_COL32( 80,  80,  80, 200);

    // Accent (gold)
    t.Accent            = IM_COL32(218, 185, 107, 255);
    t.AccentHover       = IM_COL32(240, 206, 125, 255);
    t.AccentActive      = IM_COL32(200, 170,  90, 255);
    t.AccentText        = IM_COL32( 18,  18,  18, 255);
    t.AccentFaded       = IM_COL32(218, 185, 107,  32);
    t.AccentRing        = IM_COL32(218, 185, 107,  28);
    t.AccentRingInner   = IM_COL32(218, 185, 107,  14);
    t.LobbyAvatar       = IM_COL32(118, 110, 215, 255);

    // Text
    t.TextPrimary       = IM_COL32(210, 210, 210, 255);
    t.TextSecondary     = IM_COL32(140, 140, 140, 255);
    t.TextMuted         = IM_COL32(100, 100, 100, 255);
    t.TextSystem        = IM_COL32(115, 115, 115, 255);
    t.TextTitlebar      = IM_COL32(185, 185, 185, 220);
    t.TextTagline       = IM_COL32(160, 160, 160,  35);

    // Status
    t.StatusOnline      = IM_COL32( 76, 200,  76, 255);
    t.StatusAway        = IM_COL32(210, 170,  50, 220);
    t.StatusPending     = IM_COL32(218, 185, 107, 255);
    t.StatusOffline     = IM_COL32(180,  65,  65, 255);
    t.StatusDotInactive = IM_COL32(130, 130, 130, 140);

    // Ghost buttons (titlebar)
    t.GhostBtnBg       = IM_COL32(  0,   0,   0,   0);
    t.GhostBtnHover    = IM_COL32(255, 255, 255,  20);
    t.GhostBtnActive   = IM_COL32(255, 255, 255,  35);

    // Danger buttons (leave)
    t.DangerBtn         = IM_COL32(140,  50,  50, 200);
    t.DangerBtnHover    = IM_COL32(180,  60,  60, 230);
    t.DangerBtnActive   = IM_COL32(200,  70,  70, 255);
    t.DangerBtnText     = IM_COL32(255, 255, 255, 255);

    // Send button (lighter blue-purple for dark mode)
    t.SendBtn           = IM_COL32(118, 110, 215, 255);
    t.SendBtnHover      = IM_COL32(140, 132, 235, 255);
    t.SendBtnActive     = IM_COL32(100,  92, 188, 255);
    t.SendBtnText       = IM_COL32(255, 255, 255, 255);
    t.SendBtnMuted      = IM_COL32( 55,  55,  60, 255);
    t.SendBtnMutedHover = IM_COL32( 65,  65,  70, 255);

    // Decline buttons
    t.DeclineBtn        = IM_COL32( 60,  60,  60, 200);
    t.DeclineBtnHover   = IM_COL32( 80,  50,  50, 220);
    t.DeclineBtnActive  = IM_COL32(100,  50,  50, 255);

    // Logout
    t.LogoutBtnHover    = IM_COL32(200,  80,  80,  30);
    t.LogoutBtnActive   = IM_COL32(200,  80,  80,  55);
    t.LogoutIcon        = IM_COL32(190,  90,  90, 200);
    t.LogoutIconHover   = IM_COL32(240,  90,  90, 255);

    // Scrollbar
    t.ScrollBg          = IM_COL32(  0,   0,   0,   0);
    t.ScrollGrab        = IM_COL32(255, 255, 255,  30);
    t.ScrollGrabHover   = IM_COL32(255, 255, 255,  55);
    t.ScrollGrabActive  = IM_COL32(255, 255, 255,  80);

    // Toggle icon (matches send button purple)
    t.ToggleIconActive  = IM_COL32(118, 110, 215, 230);
    t.ToggleIconInactive= IM_COL32(140, 140, 140, 130);

    // Misc
    t.UnreadDot         = IM_COL32(218, 185, 107, 255);
    t.AvatarImageTint   = IM_COL32(255, 255, 255, 255);
    t.AvatarLetterCol   = IM_COL32(255, 255, 255, 255);
    t.ConvoTitleCol     = IM_COL32(210, 210, 210, 255);
    t.ConvoPreviewCol   = IM_COL32(130, 130, 130, 255);
    t.ConvoTimeCol      = IM_COL32(110, 110, 110, 255);
    t.BubbleOutline     = IM_COL32( 48,  48,  48, 255);

    // ImGui overrides
    t.ImGuiWindowBg     = t.BgTitlebar;
    t.ImGuiChildBg      = t.BgPanel;
    t.ImGuiPopupBg      = t.BgPopup;
    t.ImGuiBorder       = t.WindowBorder;
    t.ImGuiText         = t.TextPrimary;

    return t;
}

inline ThemeData LightPalette() {
    ThemeData t{};

    // Backgrounds
    t.BgWindow          = IM_COL32(240, 240, 242, 255);
    t.BgTitlebar        = IM_COL32(252, 252, 253, 255);
    t.BgPanel           = IM_COL32(240, 240, 242, 255);
    t.BgItemSelected    = IM_COL32(225, 225, 228, 255);
    t.BgItemHovered     = IM_COL32(232, 232, 235, 255);
    t.BgOwnBubble       = IM_COL32(200, 218, 240, 255);
    t.BgPeerBubble      = IM_COL32(228, 228, 232, 255);
    t.BgInput           = IM_COL32(255, 255, 255, 255);
    t.BgPopup           = IM_COL32(250, 250, 252, 250);
    t.BgPopupAlt        = IM_COL32(248, 248, 250, 250);
    t.BgFrame           = IM_COL32(232, 232, 235, 255);
    t.BgFrameHovered    = IM_COL32(222, 222, 226, 255);
    t.BgFrameActive     = IM_COL32(212, 212, 218, 255);

    // Borders & lines
    t.Separator         = IM_COL32(210, 210, 214, 255);
    t.Divider           = IM_COL32(210, 210, 214, 255);
    t.InputBorder       = IM_COL32(188, 188, 194, 255);
    t.InputShadow       = IM_COL32(  0,   0,   0,  12);
    t.ModalBorder       = IM_COL32(195, 195, 200, 220);
    t.WindowBorder      = IM_COL32(200, 200, 205, 255);
    t.IconOutline       = IM_COL32(175, 175, 180, 200);

    // Accent (slightly deeper gold for contrast on light)
    t.Accent            = IM_COL32(178, 142,  58, 255);
    t.AccentHover       = IM_COL32(198, 162,  72, 255);
    t.AccentActive      = IM_COL32(158, 126,  48, 255);
    t.AccentText        = IM_COL32(255, 255, 255, 255);
    t.AccentFaded       = IM_COL32(178, 142,  58,  25);
    t.AccentRing        = IM_COL32(178, 142,  58,  35);
    t.AccentRingInner   = IM_COL32(178, 142,  58,  18);
    t.LobbyAvatar       = IM_COL32( 95,  85, 200, 255);

    // Text
    t.TextPrimary       = IM_COL32( 28,  28,  30, 255);
    t.TextSecondary     = IM_COL32( 95,  95, 100, 255);
    t.TextMuted         = IM_COL32(145, 145, 150, 255);
    t.TextSystem        = IM_COL32(125, 125, 130, 255);
    t.TextTitlebar      = IM_COL32( 55,  55,  60, 230);
    t.TextTagline       = IM_COL32( 80,  80,  85,  50);

    // Status (same hues, adjusted for light bg)
    t.StatusOnline      = IM_COL32( 50, 170,  50, 255);
    t.StatusAway        = IM_COL32(195, 155,  30, 230);
    t.StatusPending     = IM_COL32(178, 142,  58, 255);
    t.StatusOffline     = IM_COL32(200,  60,  60, 255);
    t.StatusDotInactive = IM_COL32(160, 160, 165, 180);

    // Ghost buttons (titlebar)
    t.GhostBtnBg       = IM_COL32(  0,   0,   0,   0);
    t.GhostBtnHover    = IM_COL32(  0,   0,   0,  15);
    t.GhostBtnActive   = IM_COL32(  0,   0,   0,  30);

    // Danger buttons (leave) — distinct light style
    t.DangerBtn         = IM_COL32(220,  60,  60, 225);
    t.DangerBtnHover    = IM_COL32(235,  75,  75, 245);
    t.DangerBtnActive   = IM_COL32(245,  85,  85, 255);
    t.DangerBtnText     = IM_COL32(255, 255, 255, 255);

    // Send button (blue-purple)
    t.SendBtn           = IM_COL32( 95,  85, 200, 255);
    t.SendBtnHover      = IM_COL32(115, 105, 225, 255);
    t.SendBtnActive     = IM_COL32( 80,  70, 175, 255);
    t.SendBtnText       = IM_COL32(255, 255, 255, 255);
    t.SendBtnMuted      = IM_COL32(215, 215, 220, 255);
    t.SendBtnMutedHover = IM_COL32(200, 200, 208, 255);

    // Decline buttons
    t.DeclineBtn        = IM_COL32(180, 180, 185, 220);
    t.DeclineBtnHover   = IM_COL32(200, 160, 160, 235);
    t.DeclineBtnActive  = IM_COL32(210, 140, 140, 255);

    // Logout
    t.LogoutBtnHover    = IM_COL32(200,  80,  80,  25);
    t.LogoutBtnActive   = IM_COL32(200,  80,  80,  50);
    t.LogoutIcon        = IM_COL32(180,  70,  70, 210);
    t.LogoutIconHover   = IM_COL32(220,  60,  60, 255);

    // Scrollbar
    t.ScrollBg          = IM_COL32(  0,   0,   0,   0);
    t.ScrollGrab        = IM_COL32(  0,   0,   0,  25);
    t.ScrollGrabHover   = IM_COL32(  0,   0,   0,  50);
    t.ScrollGrabActive  = IM_COL32(  0,   0,   0,  75);

    // Toggle icon (matches send button purple)
    t.ToggleIconActive  = IM_COL32( 95,  85, 200, 240);
    t.ToggleIconInactive= IM_COL32(130, 130, 135, 160);

    // Misc
    t.UnreadDot         = IM_COL32(178, 142,  58, 255);
    t.AvatarImageTint   = IM_COL32(255, 255, 255, 255);
    t.AvatarLetterCol   = IM_COL32(255, 255, 255, 255);
    t.ConvoTitleCol     = IM_COL32( 35,  35,  38, 255);
    t.ConvoPreviewCol   = IM_COL32(110, 110, 115, 255);
    t.ConvoTimeCol      = IM_COL32(140, 140, 145, 255);
    t.BubbleOutline     = IM_COL32(210, 210, 214, 255);

    // ImGui overrides
    t.ImGuiWindowBg     = t.BgTitlebar;
    t.ImGuiChildBg      = t.BgPanel;
    t.ImGuiPopupBg      = t.BgPopup;
    t.ImGuiBorder       = t.WindowBorder;
    t.ImGuiText         = t.TextPrimary;

    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Global theme accessor (header-only, inline storage)
// ─────────────────────────────────────────────────────────────────────────────

enum class ThemeMode : uint8_t { Dark, Light };

struct Theme {
    // Access the current palette
    static const ThemeData& Get() { return Instance().Data; }

    // Switch between dark and light
    static void SetMode(ThemeMode mode) {
        auto& s = Instance();
        s.Mode = mode;
        s.Data = (mode == ThemeMode::Dark) ? DarkPalette() : LightPalette();
    }

    static void Toggle() {
        SetMode(GetMode() == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark);
    }

    static ThemeMode GetMode() { return Instance().Mode; }
    static bool      IsDark()  { return GetMode() == ThemeMode::Dark; }

private:
    ThemeData Data = DarkPalette();
    ThemeMode Mode = ThemeMode::Dark;

    static Theme& Instance() {
        static Theme s;
        return s;
    }
};

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_THEME_H