// ScreenChooser.swift
//
// Which display the HUD appears on. Pure (geometry in, index out) so it is
// tested without AppKit.
//
// Why this exists: the first version used NSScreen.main. That is not "the main
// display"; it is "the screen that currently has keyboard focus", and it
// changes as the user clicks around. On a two-display desk the pill landed in
// the far corner of an ultrawide while its owner was looking at the laptop.

import CoreGraphics
import Foundation

enum ScreenPolicy: String, Equatable {
    /// The display the mouse pointer is on when the HUD appears: the best cheap
    /// guess at where the user is looking.
    case pointer
    /// Always the primary display (the one with the menu bar; first in AppKit's list).
    case primary
}

enum ScreenChooser {
    /// `screens` are the displays' frames in AppKit's global coordinates,
    /// primary first, as NSScreen.screens gives them. Returns an index into it,
    /// or nil only if there are no screens at all.
    static func choose(policy: ScreenPolicy, pointer: CGPoint, screens: [CGRect]) -> Int? {
        guard !screens.isEmpty else { return nil }
        if policy == .pointer {
            // Half-open on purpose (min inclusive, max exclusive), so a point on
            // the edge two displays share belongs to exactly one of them.
            // CGRect.contains is documented that way.
            if let index = screens.firstIndex(where: { $0.contains(pointer) }) {
                return index
            }
        }
        return 0   // .primary, or a pointer that is on no display (it can be, in the gaps of an uneven arrangement)
    }
}
