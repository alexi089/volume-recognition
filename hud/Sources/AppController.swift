// AppController.swift
//
// Owns the panel and executes the commands produced by VisibilityModel. It also
// diffs incoming content so the ~30 messages/s the producer sends do minimal
// work: identical messages are dropped, the glyph is rebuilt only when its
// symbol name changes, and the pill is resized/repositioned only when a
// size-affecting change occurs (the error line appearing/disappearing, or the
// first show) or when the screen configuration changes. All UI work is
// @MainActor. Reduce Motion disables the pulse and makes transitions instant.

import AppKit

@MainActor
final class AppController: NSObject, NSApplicationDelegate {

    private let panel: HudPanel
    private let content: HudContentView
    private var visibility = VisibilityModel(linger: 1.0)

    private var reduceMotion: Bool
    private var isOrderedIn = false
    private var hideWork: DispatchWorkItem?

    // Content diff state.
    private var lastApplied: HudMessage?
    private var lastGlyphName: String?
    private var lastAxisSymbol: String?

    // Motion budget: 150-300 ms, ease-in-out.
    private let fadeInDuration = 0.20
    private let fadeOutDuration = 0.24

    // Top-left placement.
    private let edgeMargin: CGFloat = 18

    private let screenPolicy: ScreenPolicy

    init(options: HudOptions) {
        screenPolicy = options.screen
        let initial = NSRect(x: 0, y: 0, width: 240, height: 80)
        content = HudContentView(frame: initial, glassOpacity: options.glassOpacity)
        panel = HudPanel(contentRect: initial)
        reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        super.init()

        panel.contentView = content
        panel.alphaValue = 0            // hidden while idle

        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(accessibilityOptionsChanged),
            name: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil)

        NotificationCenter.default.addObserver(
            self, selector: #selector(screenParametersChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)
    }

    @objc private func accessibilityOptionsChanged() {
        reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        if reduceMotion { stopPulse() }             // freeze any running pulse
        content.applyTransparency()                 // "Reduce transparency" may have changed too
    }

    @objc private func screenParametersChanged() {
        if isOrderedIn { positionTopLeft() }        // reposition only; size unchanged
    }

    // MARK: - Message handling

    func apply(_ message: HudMessage) {
        if lastApplied == message { return }        // drop identical repeats (MED-4)
        lastApplied = message

        let percent = Int((message.volume * 100).rounded())
        let isError = message.error != nil

        // Glyph: rebuild only when the symbol name changes. Slash exactly at 0%.
        let glyphName = Self.symbolName(percent: percent, isError: isError)
        if glyphName != lastGlyphName {
            content.setGlyph(symbolName: glyphName, isError: isError)
            lastGlyphName = glyphName
        }

        content.setPercent("\(percent)%")

        // Directional arrow: rebuild only when it changes.
        let axisSymbol = Self.axisSymbol(for: message.axis)
        if axisSymbol != lastAxisSymbol {
            content.setAxisArrow(symbolName: axisSymbol)
            lastAxisSymbol = axisSymbol
        }

        // Error line: only a shown/hidden toggle affects size.
        let sizeChanged = content.setError(message.error)

        // Bar level always; animate only once armed (avoids stacking with the
        // fade-in + pulse during arming, LOW-6). Emphasis reflects the axis.
        let animateBar = message.state == .armed && !reduceMotion && isOrderedIn
        content.barView.setLevel(CGFloat(message.volume), animated: animateBar)
        content.barView.setEmphasis(full: message.axis != nil,
                                    animated: !reduceMotion && isOrderedIn)

        if sizeChanged && isOrderedIn { resizeAndReposition() }

        send(.message(state: message.state, now: Self.monotonicNow()))
    }

    /// EOF / signal / parent gone: quit cleanly.
    func requestQuit() {
        NSApp.terminate(nil)
    }

    // MARK: - Command execution

    private func send(_ event: VisibilityEvent) {
        for command in visibility.advance(event) { execute(command) }
    }

    private func execute(_ command: VisibilityCommand) {
        switch command {
        case .fadeIn:                            performFadeIn()
        case .fadeOut(let token):                performFadeOut(token: token)
        case .cancelFade:                        cancelScheduledHide()
        case .startPulse:                        startPulse()
        case .stopPulse:                         stopPulse()
        case .scheduleHide(let after, let token): scheduleHide(after: after, token: token)
        case .hideNow:                           performHideNow()
        }
    }

    private func performFadeIn() {
        cancelScheduledHide()
        if !isOrderedIn {
            resizeAndReposition()
            panel.alphaValue = reduceMotion ? 1 : 0
            panel.orderFrontRegardless()          // appear without activating
            isOrderedIn = true
        }
        if reduceMotion {
            panel.alphaValue = 1
        } else {
            // Retargets any in-flight fade-out up to full alpha; the old fade's
            // completion is neutralised by the model's token guard (HIGH-2).
            NSAnimationContext.runAnimationGroup { ctx in
                ctx.duration = fadeInDuration
                ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                panel.animator().alphaValue = 1
            }
        }
    }

    private func performFadeOut(token: Int) {
        if reduceMotion {
            panel.alphaValue = 0
            send(.fadeFinished(token: token))
            return
        }
        NSAnimationContext.runAnimationGroup({ ctx in
            ctx.duration = fadeOutDuration
            ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            panel.animator().alphaValue = 0
        }, completionHandler: { [weak self] in
            self?.send(.fadeFinished(token: token))
        })
    }

    private func performHideNow() {
        panel.orderOut(nil)
        panel.alphaValue = 0
        isOrderedIn = false
    }

    private func scheduleHide(after: Double, token: Int) {
        cancelScheduledHide()
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated { self?.send(.hideTimerFired(token: token)) }
        }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + after, execute: work)
    }

    private func cancelScheduledHide() {
        hideWork?.cancel()
        hideWork = nil
    }

    // MARK: - Pulse (subtle opacity breathing on the glyph)

    private func startPulse() {
        guard !reduceMotion else { return }
        guard content.glyphView.layer?.animation(forKey: "pulse") == nil else { return }
        let pulse = CABasicAnimation(keyPath: "opacity")
        pulse.fromValue = 1.0
        pulse.toValue = 0.55
        pulse.duration = 0.75
        pulse.autoreverses = true
        pulse.repeatCount = .infinity
        pulse.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        content.glyphView.layer?.add(pulse, forKey: "pulse")
    }

    private func stopPulse() {
        content.glyphView.layer?.removeAnimation(forKey: "pulse")
    }

    // MARK: - Geometry (top-left of the main screen's visible area)

    private func resizeAndReposition() {
        panel.setContentSize(content.fittingPanelSize())
        positionTopLeft()
    }

    private func positionTopLeft() {
        // Deliberately NOT NSScreen.main, which is "the screen with keyboard
        // focus" and put the pill on the wrong display. See ScreenChooser.
        let screens = NSScreen.screens
        guard let index = ScreenChooser.choose(policy: screenPolicy,
                                               pointer: NSEvent.mouseLocation,
                                               screens: screens.map(\.frame)) else { return }
        let vf = screens[index].visibleFrame        // excludes menu bar and Dock
        let frame = panel.frame
        // The visible pill is inset from the panel by shadowMargin on all sides;
        // offset so the PILL sits at edgeMargin from the top-left, not the panel.
        let x = vf.minX + edgeMargin - content.shadowMargin
        let y = vf.maxY - edgeMargin - frame.height + content.shadowMargin
        panel.setFrameOrigin(NSPoint(x: x.rounded(), y: y.rounded()))
    }

    // MARK: - Symbol selection

    /// Speaker variant by rounded percentage (slash EXACTLY at 0%, LOW-5), or a
    /// warning-badge variant on error (a shape change, not just a tint).
    static func symbolName(percent: Int, isError: Bool) -> String {
        if isError {
            for candidate in ["speaker.badge.exclamationmark.fill",
                              "speaker.badge.exclamationmark",
                              "exclamationmark.triangle.fill"] {
                if NSImage(systemSymbolName: candidate, accessibilityDescription: nil) != nil {
                    return candidate
                }
            }
        }
        switch percent {
        case ..<1:   return "speaker.slash.fill"
        case ..<25:  return "speaker.fill"
        case ..<50:  return "speaker.wave.1.fill"
        case ..<75:  return "speaker.wave.2.fill"
        default:     return "speaker.wave.3.fill"
        }
    }

    static func axisSymbol(for axis: HudAxis?) -> String? {
        switch axis {
        case .x: return "arrow.left.and.right"
        case .y: return "arrow.up.and.down"
        case nil: return nil
        }
    }

    /// A monotonic clock (seconds); unaffected by wall-clock changes.
    static func monotonicNow() -> Double {
        ProcessInfo.processInfo.systemUptime
    }
}
