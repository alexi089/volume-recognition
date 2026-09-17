// HudContentView.swift
//
// The visual content of the HUD: a compact glass-style PILL (NSVisualEffectView
// with continuous rounded corners, a hairline border and a soft drop shadow)
// containing a speaker glyph, a slim animated level bar, an optional tiny
// directional arrow, and a percentage. An error message, when present, appears
// on a compact second line and the pill grows to fit it.
//
// NOTE ON NAMING: this is a "glass-style" HUD built on NSVisualEffectView
// (material .hudWindow). It is NOT Apple's macOS 26 "Liquid Glass" — those APIs
// do not exist in this SDK. See README for what would change on macOS 26.
//
// Layout, outermost to innermost:
//   self (transparent)            <- panel contentView; holds the shadow margin
//     card (border + soft shadow, NOT clipped)
//       glass (NSVisualEffectView, rounded + clipped)
//         vStack
//           hRow: [glyph] [level bar] [arrow slot] [percent]
//           errorLabel (hidden unless there is an error)
//
// The arrow slot is a fixed width and always present, so toggling the direction
// arrow never changes the pill's size.

import AppKit

@MainActor
final class HudContentView: NSView {

    // Sub-elements the controller drives.
    let glyphView = NSImageView()
    let barView = LevelBarView()
    let arrowView = NSImageView()
    let percentLabel = NSTextField(labelWithString: "0%")
    let errorLabel = NSTextField(labelWithString: "")

    private let card = NSView()
    private let glass = NSVisualEffectView()
    private let vStack = NSStackView()
    private let hRow = NSStackView()

    // Compact geometry.
    private let glassWidth: CGFloat = 168
    private let hInset: CGFloat = 12
    private let vInset: CGFloat = 7
    private let cornerRadius: CGFloat = 16          // continuous corners, scaled for the pill
    private let glyphPointSize: CGFloat = 13
    /// Transparent margin around the card so the soft shadow is not clipped and
    /// the rounded corners stay intact.
    let shadowMargin: CGFloat = 24

    /// 1.0 is the stock material; lower is clearer glass. See HudOptions.
    private let glassOpacity: Double
    private let highlight = CAGradientLayer()

    init(frame frameRect: NSRect, glassOpacity: Double) {
        self.glassOpacity = glassOpacity
        super.init(frame: frameRect)
        setup()
    }

    required init?(coder: NSCoder) {
        fatalError("HudContentView is only created in code")
    }

    private func setup() {
        wantsLayer = true

        // --- card: carries the border + shadow, must NOT clip its own shadow ---
        card.wantsLayer = true
        card.translatesAutoresizingMaskIntoConstraints = false
        if let cl = card.layer {
            cl.cornerRadius = cornerRadius
            cl.cornerCurve = .continuous
            cl.masksToBounds = false
            cl.borderWidth = 1
            cl.shadowColor = NSColor.black.cgColor
            cl.shadowOpacity = 0.14
            cl.shadowRadius = 12
            cl.shadowOffset = CGSize(width: 0, height: -6)   // downward (non-flipped)
        }

        // --- glass: the translucent material, rounded + clipped ---
        glass.material = .hudWindow
        glass.blendingMode = .behindWindow
        glass.state = .active
        // NSVisualEffectView already becomes more opaque under "Reduce
        // transparency" automatically; nothing extra is needed.
        glass.wantsLayer = true
        glass.translatesAutoresizingMaskIntoConstraints = false
        if let gl = glass.layer {
            gl.cornerRadius = cornerRadius
            gl.cornerCurve = .continuous
            gl.masksToBounds = true
        }

        // --- glyph ---
        glyphView.translatesAutoresizingMaskIntoConstraints = false
        glyphView.wantsLayer = true                 // needed for the arming pulse
        glyphView.imageScaling = .scaleProportionallyUpOrDown
        glyphView.setContentHuggingPriority(.required, for: .horizontal)

        // --- directional arrow (fixed-width slot; empty until a direction) ---
        arrowView.translatesAutoresizingMaskIntoConstraints = false
        arrowView.imageScaling = .scaleProportionallyDown
        arrowView.contentTintColor = .secondaryLabelColor
        arrowView.setContentHuggingPriority(.required, for: .horizontal)

        // --- percentage ---
        percentLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        percentLabel.textColor = .labelColor
        percentLabel.alignment = .right
        percentLabel.translatesAutoresizingMaskIntoConstraints = false
        percentLabel.setContentHuggingPriority(.required, for: .horizontal)
        percentLabel.setContentCompressionResistancePriority(.required, for: .horizontal)

        // --- error line ---
        errorLabel.font = .systemFont(ofSize: 10.5, weight: .regular)
        errorLabel.textColor = .secondaryLabelColor
        errorLabel.maximumNumberOfLines = 2
        errorLabel.lineBreakMode = .byWordWrapping
        errorLabel.cell?.wraps = true
        errorLabel.cell?.isScrollable = false
        errorLabel.preferredMaxLayoutWidth = glassWidth
        errorLabel.isHidden = true

        // --- rows ---
        hRow.orientation = .horizontal
        hRow.alignment = .centerY
        hRow.spacing = 6
        hRow.translatesAutoresizingMaskIntoConstraints = false
        hRow.addArrangedSubview(glyphView)
        hRow.addArrangedSubview(barView)
        hRow.addArrangedSubview(arrowView)
        hRow.addArrangedSubview(percentLabel)

        vStack.orientation = .vertical
        vStack.alignment = .leading
        vStack.distribution = .fill
        vStack.spacing = 5
        vStack.translatesAutoresizingMaskIntoConstraints = false
        vStack.addArrangedSubview(hRow)
        vStack.addArrangedSubview(errorLabel)

        // --- assemble ---
        // The content sits ABOVE the glass as its sibling, not inside it, so the
        // material can be made more see-through without fading the glyph, the
        // bar and the number along with it.
        addSubview(card)
        card.addSubview(glass)
        card.addSubview(vStack)

        // What makes a translucent panel read as glass rather than as a grey
        // smudge is light catching its top edge: a soft white gradient over the
        // upper half, under the content.
        highlight.colors = [NSColor.white.withAlphaComponent(0.22).cgColor,
                            NSColor.white.withAlphaComponent(0.0).cgColor]
        highlight.startPoint = CGPoint(x: 0.5, y: 1.0)   // top (layer space is not flipped)
        highlight.endPoint = CGPoint(x: 0.5, y: 0.45)
        highlight.cornerRadius = cornerRadius
        highlight.cornerCurve = .continuous
        highlight.masksToBounds = true
        glass.layer?.addSublayer(highlight)

        applyTransparency()

        NSLayoutConstraint.activate([
            card.leadingAnchor.constraint(equalTo: leadingAnchor, constant: shadowMargin),
            card.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -shadowMargin),
            card.topAnchor.constraint(equalTo: topAnchor, constant: shadowMargin),
            card.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -shadowMargin),

            glass.leadingAnchor.constraint(equalTo: card.leadingAnchor),
            glass.trailingAnchor.constraint(equalTo: card.trailingAnchor),
            glass.topAnchor.constraint(equalTo: card.topAnchor),
            glass.bottomAnchor.constraint(equalTo: card.bottomAnchor),

            vStack.leadingAnchor.constraint(equalTo: glass.leadingAnchor, constant: hInset),
            vStack.trailingAnchor.constraint(equalTo: glass.trailingAnchor, constant: -hInset),
            vStack.topAnchor.constraint(equalTo: glass.topAnchor, constant: vInset),
            vStack.bottomAnchor.constraint(equalTo: glass.bottomAnchor, constant: -vInset),
            vStack.widthAnchor.constraint(equalToConstant: glassWidth),

            glyphView.widthAnchor.constraint(equalToConstant: 18),
            glyphView.heightAnchor.constraint(equalToConstant: 16),
            arrowView.widthAnchor.constraint(equalToConstant: 13),
            arrowView.heightAnchor.constraint(equalToConstant: 13),
            percentLabel.widthAnchor.constraint(equalToConstant: 34),
            barView.heightAnchor.constraint(equalToConstant: 4),

            hRow.widthAnchor.constraint(equalTo: vStack.widthAnchor),
            errorLabel.widthAnchor.constraint(equalTo: vStack.widthAnchor),
        ])

        barView.setContentHuggingPriority(.defaultLow, for: .horizontal)
        barView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        applyBorderColor()
    }

    /// "Reduce transparency" means the user has asked for solid surfaces: full
    /// material, no highlight, whatever --opacity says.
    func applyTransparency() {
        let reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceTransparency
        glass.alphaValue = reduce ? 1.0 : CGFloat(glassOpacity)
        highlight.isHidden = reduce
    }

    override func layout() {
        super.layout()
        // Layers do not take part in Auto Layout; keep the highlight over the glass.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        highlight.frame = glass.bounds
        CATransaction.commit()
        if let cl = card.layer {
            cl.shadowPath = CGPath(roundedRect: card.bounds,
                                   cornerWidth: cornerRadius,
                                   cornerHeight: cornerRadius,
                                   transform: nil)
        }
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        applyBorderColor()
    }

    /// The size the panel content should be, including the shadow margin.
    func fittingPanelSize() -> NSSize {
        layoutSubtreeIfNeeded()
        return fittingSize
    }

    // MARK: - Granular setters (the controller diffs and calls only what changed)

    /// Rebuild the glyph image. Call only when the symbol name or error state
    /// actually changes. On error the glyph SHAPE changes (a badge symbol) in
    /// addition to the tint, so colour is never the only signal. (The resize
    /// symbol has no badge variant: there the error LINE is the second signal.)
    /// `what` names the thing for VoiceOver: "volume" or "window size".
    func setGlyph(symbolName: String, isError: Bool, what: String) {
        let config = NSImage.SymbolConfiguration(pointSize: glyphPointSize, weight: .semibold)
        let image = NSImage(systemSymbolName: symbolName,
                            accessibilityDescription: isError ? "\(what), error" : what)?
            .withSymbolConfiguration(config)
        image?.isTemplate = true
        glyphView.image = image
        glyphView.contentTintColor = isError ? .systemOrange : .labelColor
    }

    func setPercent(_ text: String) {
        percentLabel.stringValue = text
    }

    /// Set (or clear) the tiny directional arrow. The slot is fixed width, so
    /// this never resizes the pill.
    func setAxisArrow(symbolName: String?) {
        if let name = symbolName {
            let config = NSImage.SymbolConfiguration(pointSize: 9, weight: .semibold)
            let image = NSImage(systemSymbolName: name, accessibilityDescription: "slide direction")?
                .withSymbolConfiguration(config)
            image?.isTemplate = true
            arrowView.image = image
        } else {
            arrowView.image = nil
        }
    }

    /// Show/hide the error line. Returns true if the shown/hidden state changed
    /// (i.e. the pill's size changed and the caller must resize/reposition).
    @discardableResult
    func setError(_ text: String?) -> Bool {
        let show = (text?.isEmpty == false)
        let changed = (show != !errorLabel.isHidden)
        if let text, show { errorLabel.stringValue = text } else { errorLabel.stringValue = "" }
        errorLabel.isHidden = !show
        return changed
    }

    private func applyBorderColor() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            let isDark = effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            let border = isDark
                ? NSColor(white: 1.0, alpha: 0.22)
                : NSColor(white: 0.0, alpha: 0.18)
            card.layer?.borderColor = border.cgColor
            card.layer?.shadowColor = NSColor.black.cgColor
        }
    }
}
