// HudPanel.swift
//
// A borderless, floating, non-activating panel that never takes focus, ignores
// mouse events, shows on all Spaces and over full-screen apps, and casts no
// system window shadow (the content view draws its own soft shadow).

import AppKit

final class HudPanel: NSPanel {

    init(contentRect: NSRect) {
        super.init(contentRect: contentRect,
                   styleMask: [.borderless, .nonactivatingPanel],
                   backing: .buffered,
                   defer: false)

        isFloatingPanel = true
        becomesKeyOnlyIfNeeded = true
        hidesOnDeactivate = false
        isMovableByWindowBackground = false

        // Sit above ordinary and floating windows, roughly where system HUDs live.
        level = .statusBar

        // Show across every Space and join another app's full-screen space.
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary,
                              .stationary, .ignoresCycle]

        // Transparent window; the content view provides the glass + custom shadow.
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false

        // Display only: never intercept clicks.
        ignoresMouseEvents = true
    }

    // Never become key or main -> never steals focus from the active app.
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}
