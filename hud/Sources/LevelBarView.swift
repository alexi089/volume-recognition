// LevelBarView.swift
//
// A rounded, horizontal level bar (track + fill) that animates smoothly between
// values. Colours are semantic (labelColor / tertiaryLabelColor) and re-resolved
// on appearance changes so the track stays visible in light mode and the fill
// reads in both appearances.
//
// `emphasis` conveys the axis hint: reduced while armed with no decided
// direction (axis == nil), full once the direction is known. The bar is never
// rotated; only its fill opacity changes.

import AppKit

final class LevelBarView: NSView {

    private let trackLayer = CALayer()
    private let fillLayer = CALayer()

    /// Current fill level, 0...1.
    private(set) var level: CGFloat = 0
    /// Full emphasis (direction known) vs reduced (waiting for a direction).
    private(set) var fullEmphasis = true

    private let fullFillOpacity: Float = 1.0
    private let reducedFillOpacity: Float = 0.4

    // Motion within the 150-300 ms budget, ease-in-out.
    private let animationDuration: CFTimeInterval = 0.22

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        commonInit()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        wantsLayer = true
        layer?.addSublayer(trackLayer)
        layer?.addSublayer(fillLayer)
        trackLayer.masksToBounds = true
        fillLayer.masksToBounds = true
        fillLayer.opacity = fullFillOpacity
        applyColors()
    }

    override var isFlipped: Bool { false }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let h = bounds.height
        let radius = h / 2
        trackLayer.frame = bounds
        trackLayer.cornerRadius = radius
        fillLayer.frame = CGRect(x: 0, y: 0, width: bounds.width * level, height: h)
        fillLayer.cornerRadius = radius
        CATransaction.commit()
    }

    /// Set the fill level. `animated == false` (Reduce Motion) snaps instantly.
    func setLevel(_ newLevel: CGFloat, animated: Bool) {
        let clamped = min(max(newLevel, 0), 1)
        level = clamped
        let target = CGRect(x: 0, y: 0, width: bounds.width * clamped, height: bounds.height)
        CATransaction.begin()
        if animated {
            CATransaction.setAnimationDuration(animationDuration)
            CATransaction.setAnimationTimingFunction(CAMediaTimingFunction(name: .easeInEaseOut))
        } else {
            CATransaction.setDisableActions(true)
        }
        fillLayer.frame = target
        CATransaction.commit()
    }

    /// Set the emphasis (direction known vs waiting). No-op if unchanged.
    func setEmphasis(full: Bool, animated: Bool) {
        guard full != fullEmphasis else { return }
        fullEmphasis = full
        let target = full ? fullFillOpacity : reducedFillOpacity
        CATransaction.begin()
        if animated {
            CATransaction.setAnimationDuration(animationDuration)
            CATransaction.setAnimationTimingFunction(CAMediaTimingFunction(name: .easeInEaseOut))
        } else {
            CATransaction.setDisableActions(true)
        }
        fillLayer.opacity = target
        CATransaction.commit()
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        applyColors()
    }

    private func applyColors() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            trackLayer.backgroundColor = NSColor.tertiaryLabelColor.cgColor
            fillLayer.backgroundColor = NSColor.labelColor.cgColor
            CATransaction.commit()
        }
    }
}
