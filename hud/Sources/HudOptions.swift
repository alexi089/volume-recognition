// HudOptions.swift
//
// Command-line options. Foundation-only and pure, so it is tested in the plain
// test executable alongside the parser and the visibility reducer.
//
//   volume-hud [--opacity 0.15...1.0]
//
// Nobody who worked on this HUD could see it (no screen access from the build
// session), so how see-through the glass should be is left to the person
// looking at it rather than guessed.

import Foundation

struct HudOptions: Equatable {
    /// How much of the blur material shows, 1.0 being the stock `.hudWindow`
    /// look. Lower lets more of what is behind the pill through, which reads as
    /// clearer glass. The content (glyph, bar, number) is never faded by this.
    static let defaultGlassOpacity = 0.6
    static let opacityRange = 0.15...1.0

    var glassOpacity: Double = HudOptions.defaultGlassOpacity
    /// Problems found while parsing. The HUD still starts; these go to stderr.
    var warnings: [String] = []

    static func parse(_ arguments: [String]) -> HudOptions {
        var options = HudOptions()
        var rest = arguments.dropFirst().makeIterator()   // [0] is the program name

        while let argument = rest.next() {
            switch argument {
            case "--opacity":
                guard let text = rest.next() else {
                    options.warnings.append("--opacity needs a value between 0.15 and 1.0")
                    break
                }
                // Double("nan") succeeds, so finiteness is checked separately.
                guard let value = Double(text), value.isFinite else {
                    options.warnings.append("--opacity \(text): not a number; using \(defaultGlassOpacity)")
                    break
                }
                options.glassOpacity = min(max(value, opacityRange.lowerBound), opacityRange.upperBound)
            default:
                options.warnings.append("unknown option \(argument)")
            }
        }
        return options
    }
}
