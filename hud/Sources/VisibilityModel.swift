// VisibilityModel.swift
//
// A pure, Foundation-only reducer for the HUD's visibility/animation lifecycle.
// It contains NO AppKit and performs NO side effects: it maps events to a list
// of commands the AppController executes, and updates its own phase. This is the
// root-cause fix for two bugs found under the real producer (which sends ~28-30
// lines EVERY frame, including a continuous stream of idle lines after release):
//
//   HIGH-1  The hide must start on the TRANSITION into idle and must NOT be
//           re-armed by the flood of subsequent idle lines. Here, only the
//           shown -> lingering transition schedules the hide; further idle
//           messages return no commands. As a belt-and-suspenders that also uses
//           the message timestamps as a clock, once the linger has elapsed the
//           next idle message itself begins the fade if the timer has not fired.
//
//   HIGH-2  Re-arming during the fade-out must end fully visible. Arriving in
//           the .fadingOut phase, arming/armed transitions to .shown and emits
//           .fadeIn (which cancels the in-flight fade and animates alpha back to
//           1). A late .fadeFinished from the cancelled fade is ignored because
//           its token no longer matches and the phase is no longer .fadingOut.
//
// Tokens make async callbacks (a scheduled hide, a fade-out completion)
// self-invalidating: only a callback whose token matches the current pending
// one, in the expected phase, has any effect.

import Foundation

/// What the controller should do. An empty command list means "do nothing".
enum VisibilityCommand: Equatable {
    case fadeIn                                   // order in if needed; animate alpha -> 1
    case fadeOut(token: Int)                      // animate alpha -> 0; report .fadeFinished(token)
    case cancelFade                               // cancel a pending scheduled-hide
    case startPulse
    case stopPulse
    case scheduleHide(after: Double, token: Int)  // fire .hideTimerFired(token) after `after`s
    case hideNow                                  // order out; fully hidden
}

/// Inputs to the reducer: a message (with its arrival time), or an async
/// callback the controller feeds back.
enum VisibilityEvent: Equatable {
    case message(state: HudState, now: Double)
    case hideTimerFired(token: Int)
    case fadeFinished(token: Int)
}

struct VisibilityModel {

    enum Phase: Equatable {
        case hidden       // not on screen (idle)
        case shown        // fully visible (arming/armed)
        case lingering    // idle received while visible; hide scheduled; still at full alpha
        case fadingOut    // fade-out animation in progress
    }

    private(set) var phase: Phase = .hidden
    private(set) var pulsing = false

    /// Seconds the final volume stays readable after release before fading out.
    let linger: Double

    private var idleSince: Double = 0
    private var hideToken = 0
    private var fadeToken = 0
    private var tokenSeq = 0

    init(linger: Double = 1.0) {
        self.linger = linger
    }

    mutating func advance(_ event: VisibilityEvent) -> [VisibilityCommand] {
        switch event {
        case .message(let state, let now):
            return handleMessage(state, now)
        case .hideTimerFired(let token):
            guard phase == .lingering, token == hideToken else { return [] }
            return beginFadeOut()
        case .fadeFinished(let token):
            guard phase == .fadingOut, token == fadeToken else { return [] }
            phase = .hidden
            return [.hideNow]
        }
    }

    // MARK: - Private

    private mutating func handleMessage(_ state: HudState, _ now: Double) -> [VisibilityCommand] {
        switch state {
        case .arming: return goVisible(wantPulse: true)
        case .armed:  return goVisible(wantPulse: false)
        case .idle:   return goIdle(now)
        }
    }

    private mutating func goVisible(wantPulse: Bool) -> [VisibilityCommand] {
        var cmds: [VisibilityCommand] = []
        switch phase {
        case .hidden:
            phase = .shown
            cmds.append(.fadeIn)
        case .lingering:
            phase = .shown
            cmds.append(.cancelFade)   // still at full alpha; just drop the pending hide
        case .fadingOut:
            phase = .shown
            cmds.append(.fadeIn)       // cancel the fade and animate back to full alpha (HIGH-2)
        case .shown:
            break                      // already visible
        }
        if wantPulse {
            if !pulsing { pulsing = true; cmds.append(.startPulse) }
        } else {
            if pulsing { pulsing = false; cmds.append(.stopPulse) }
        }
        return cmds
    }

    private mutating func goIdle(_ now: Double) -> [VisibilityCommand] {
        switch phase {
        case .hidden:
            return []                  // stay hidden while idle
        case .shown:
            phase = .lingering
            idleSince = now
            var cmds: [VisibilityCommand] = []
            if pulsing { pulsing = false; cmds.append(.stopPulse) }
            hideToken = nextToken()
            cmds.append(.scheduleHide(after: linger, token: hideToken))   // HIGH-1: only on transition
            return cmds
        case .lingering:
            // Do NOT re-arm the timer (HIGH-1). But if the linger has already
            // elapsed and the timer callback has not arrived, the message stream
            // itself starts the fade.
            if now - idleSince >= linger {
                return beginFadeOut()
            }
            return []
        case .fadingOut:
            return []                  // let the fade finish
        }
    }

    private mutating func beginFadeOut() -> [VisibilityCommand] {
        phase = .fadingOut
        fadeToken = nextToken()
        return [.fadeOut(token: fadeToken)]
    }

    private mutating func nextToken() -> Int {
        tokenSeq += 1
        return tokenSeq
    }
}
