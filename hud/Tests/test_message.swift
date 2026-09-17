// test_message.swift
//
// Plain executable tests for the Foundation-only core: the stdin line parser
// (HudMessageParser) and the visibility reducer (VisibilityModel). XCTest is
// unavailable with Command Line Tools only, so this is a hand-rolled harness: it
// prints one line per check and exits non-zero if any check fails.
//
// Built with Sources/HudMessage.swift + Sources/VisibilityModel.swift into
// build/test_message. Uses @main (with -parse-as-library) because swiftc only
// allows top-level statements in a file literally named main.swift, and the
// brief fixes this file's name as test_message.swift.
//
// Build/run:  make -C hud test

import CoreGraphics
import Foundation

/// Minimal assertion harness. State lives on the instance (not in globals) so
/// the file compiles cleanly under Swift 6 strict-concurrency checking.
final class Harness {
    private(set) var passed = 0
    private(set) var failed = 0

    func ok(_ label: String) {
        passed += 1
        print("ok    \(label)")
    }

    func fail(_ label: String, _ detail: String) {
        failed += 1
        FileHandle.standardError.write(Data("FAIL  \(label)  --  \(detail)\n".utf8))
    }

    // -- parser assertions ----------------------------------------------------

    func expectSuccess(_ line: String, _ expected: HudMessage, _ label: String) {
        switch HudMessageParser.parse(line) {
        case .success(let msg):
            if msg == expected { ok(label) }
            else { fail(label, "got \(msg), expected \(expected)") }
        case .failure(let err):
            fail(label, "expected success, got failure .\(err)")
        }
    }

    func expectFailure(_ line: String, _ expected: HudParseError, _ label: String) {
        switch HudMessageParser.parse(line) {
        case .success(let msg):
            fail(label, "expected failure .\(expected), got success \(msg)")
        case .failure(let err):
            if err == expected { ok(label) }
            else { fail(label, "got failure .\(err), expected .\(expected)") }
        }
    }

    func expectFailureMatching(_ line: String, _ matches: (HudParseError) -> Bool, _ label: String) {
        switch HudMessageParser.parse(line) {
        case .success(let msg):
            fail(label, "expected failure, got success \(msg)")
        case .failure(let err):
            if matches(err) { ok(label) }
            else { fail(label, "got unexpected failure .\(err)") }
        }
    }

    // -- generic assertions ---------------------------------------------------

    func assertTrue(_ cond: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
        if cond { ok(label) } else { fail(label, detail()) }
    }

    func assertEqual<T: Equatable>(_ got: T, _ want: T, _ label: String) {
        if got == want { ok(label) } else { fail(label, "got \(got), want \(want)") }
    }
}

@main
struct TestMain {
    static func main() {
        let h = Harness()
        parserTests(h)
        visibilityTests(h)
        optionsTests(h)
        screenTests(h)
        print("\n\(h.passed) passed, \(h.failed) failed")
        exit(h.failed > 0 ? 1 : 0)
    }

    // MARK: - Which screen

    /// The owner's real layout, where this went wrong: built-in display at the
    /// origin, an ultrawide to its right and 288 pt up. The HUD used
    /// NSScreen.main, which is "the screen with keyboard focus", not "the main
    /// display"; it landed on the ultrawide while the owner watched the built-in.
    static func screenTests(_ h: Harness) {
        func check(_ cond: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
            if cond { h.ok(label) } else { h.fail(label, detail()) }
        }
        let builtIn = CGRect(x: 0, y: 0, width: 1728, height: 1117)
        let ultrawide = CGRect(x: 1728, y: 288, width: 5120, height: 1440)
        let screens = [builtIn, ultrawide]   // AppKit lists the primary display first

        check(ScreenChooser.choose(policy: .pointer, pointer: CGPoint(x: 600, y: 500), screens: screens) == 0,
              "pointer on the built-in display -> built-in")
        check(ScreenChooser.choose(policy: .pointer, pointer: CGPoint(x: 4000, y: 900), screens: screens) == 1,
              "pointer on the ultrawide -> ultrawide")
        check(ScreenChooser.choose(policy: .primary, pointer: CGPoint(x: 4000, y: 900), screens: screens) == 0,
              "policy primary ignores the pointer")
        // Just past the built-in's right edge but below the ultrawide's bottom: on no screen.
        check(ScreenChooser.choose(policy: .pointer, pointer: CGPoint(x: 1800, y: 100), screens: screens) == 0,
              "pointer on no screen -> falls back to the primary")
        // The shared edge belongs to exactly one screen (the first that contains it).
        check(ScreenChooser.choose(policy: .pointer, pointer: CGPoint(x: 1728, y: 600), screens: screens) == 1,
              "x = 1728 is the ultrawide's first column, not the built-in's last")
        check(ScreenChooser.choose(policy: .pointer, pointer: CGPoint(x: 10, y: 10), screens: []) == nil,
              "no screens at all -> nil, not a crash")

        check(HudOptions.parse(["x"]).screen == .pointer, "default screen policy is the pointer's screen")
        check(HudOptions.parse(["x", "--screen", "primary"]).screen == .primary, "--screen primary is taken")
        let bad = HudOptions.parse(["x", "--screen", "left"])
        check(bad.screen == .pointer && bad.warnings.count == 1, "--screen left: default kept, one warning", "\(bad)")
    }

    // MARK: - Options

    static func optionsTests(_ h: Harness) {
        func check(_ cond: Bool, _ label: String, _ detail: @autoclosure () -> String = "") {
            if cond { h.ok(label) } else { h.fail(label, detail()) }
        }
        let d = HudOptions.parse(["volume-hud"])
        check(d.glassOpacity == HudOptions.defaultGlassOpacity && d.warnings.isEmpty, "no arguments: default opacity, no warnings")
        check(HudOptions.defaultGlassOpacity > 0.3 && HudOptions.defaultGlassOpacity < 0.9,
              "default glass is clearly translucent, not opaque", "\(HudOptions.defaultGlassOpacity)")

        let o = HudOptions.parse(["volume-hud", "--opacity", "0.45"])
        check(o.glassOpacity == 0.45 && o.warnings.isEmpty, "--opacity 0.45 is taken", "\(o)")

        // Clamped to a range where the pill stays both visible and glass-like.
        check(HudOptions.parse(["x", "--opacity", "0"]).glassOpacity == 0.15, "--opacity 0 clamps to 0.15 (never invisible)")
        check(HudOptions.parse(["x", "--opacity", "7"]).glassOpacity == 1.0, "--opacity 7 clamps to 1.0")

        let bad = HudOptions.parse(["x", "--opacity", "glass"])
        check(bad.glassOpacity == HudOptions.defaultGlassOpacity && bad.warnings.count == 1, "non-numeric opacity: default kept, one warning", "\(bad)")
        let nan = HudOptions.parse(["x", "--opacity", "nan"])
        check(nan.glassOpacity == HudOptions.defaultGlassOpacity && nan.warnings.count == 1, "nan opacity: default kept, one warning", "\(nan)")
        let missing = HudOptions.parse(["x", "--opacity"])
        check(missing.glassOpacity == HudOptions.defaultGlassOpacity && missing.warnings.count == 1, "--opacity with no value: one warning", "\(missing)")
        let unknown = HudOptions.parse(["x", "--frobnicate", "--opacity", "0.5"])
        check(unknown.glassOpacity == 0.5 && unknown.warnings.count == 1, "unknown option warned about, the rest still parsed", "\(unknown)")
    }

    // MARK: - Parser

    static func parserTests(_ h: Harness) {
        // Valid lines, one per state
        h.expectSuccess(#"{"state":"idle","volume":0.50,"error":null}"#,
                        HudMessage(state: .idle, volume: 0.50, error: nil), "valid idle line")
        h.expectSuccess(#"{"state":"arming","volume":0.50,"error":null}"#,
                        HudMessage(state: .arming, volume: 0.50, error: nil), "valid arming line")
        h.expectSuccess(#"{"state":"armed","volume":0.62,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.62, error: nil), "valid armed line")

        // Error field: null vs string vs empty vs whitespace vs UTF-8
        h.expectSuccess(#"{"state":"armed","volume":0.62,"error":"the output accepted the volume change but did not apply it"}"#,
                        HudMessage(state: .armed, volume: 0.62,
                                   error: "the output accepted the volume change but did not apply it"),
                        "armed line with error string")
        h.expectSuccess(#"{"state":"armed","volume":0.62,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.62, error: nil), "explicit null error becomes nil")
        h.expectSuccess(#"{"state":"armed","volume":0.62,"error":""}"#,
                        HudMessage(state: .armed, volume: 0.62, error: nil), "empty-string error is nil")
        h.expectSuccess(#"{"state":"armed","volume":0.62,"error":"   "}"#,
                        HudMessage(state: .armed, volume: 0.62, error: nil), "whitespace-only error is nil")
        h.expectSuccess("{\"state\":\"armed\",\"volume\":0.62,\"error\":\"\\t\\n \"}",
                        HudMessage(state: .armed, volume: 0.62, error: nil), "tab/newline error is nil")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"error":"café ⚠︎ 音量"}"#,
                        HudMessage(state: .armed, volume: 0.5, error: "café ⚠︎ 音量"),
                        "literal UTF-8 in error string preserved")

        // Volume clamping / numeric edge cases
        h.expectSuccess(#"{"state":"armed","volume":1.5,"error":null}"#,
                        HudMessage(state: .armed, volume: 1.0, error: nil), "volume above 1 clamps to 1")
        h.expectSuccess(#"{"state":"armed","volume":-0.2,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.0, error: nil), "volume below 0 clamps to 0")
        h.expectSuccess(#"{"state":"armed","volume":0,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.0, error: nil), "volume exactly 0 passes through")
        h.expectSuccess(#"{"state":"armed","volume":1,"error":null}"#,
                        HudMessage(state: .armed, volume: 1.0, error: nil), "integer volume 1 accepted as 1.0")
        h.expectSuccess(#"{"state":"armed","volume":0.004,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.004, error: nil), "tiny nonzero volume preserved")
        // Huge but finite integer -> parses (~1e23) and clamps to 1.0
        h.expectSuccess(#"{"state":"armed","volume":99999999999999999999999,"error":null}"#,
                        HudMessage(state: .armed, volume: 1.0, error: nil), "huge integer clamps to 1.0")
        // Positive exponent overflow: JSONSerialization rejects it as malformed
        h.expectFailureMatching(#"{"state":"armed","volume":1e400,"error":null}"#,
                                { if case .notJSON = $0 { return true }; return false },
                                "positive overflow 1e400 rejected as malformed")
        // Negative exponent overflow parses to -inf; a non-finite volume is rejected
        h.expectFailure(#"{"state":"armed","volume":-1e400,"error":null}"#,
                        .volumeNotNumber, "negative overflow -1e400 (non-finite) rejected")

        // Duplicate keys: first occurrence wins (documented behaviour)
        h.expectSuccess(#"{"state":"idle","state":"armed","volume":0.1,"error":null}"#,
                        HudMessage(state: .idle, volume: 0.1, error: nil),
                        "duplicate state key: first wins")

        // axis: x / y / null / absent / invalid (invalid must NOT reject)
        h.expectSuccess(#"{"state":"armed","volume":0.5,"axis":"x","error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: .x), "axis x parsed")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"axis":"y","error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: .y), "axis y parsed")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"axis":null,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: nil), "axis null -> nil")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: nil), "axis absent -> nil")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"axis":"z","error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: nil),
                        "invalid axis string -> nil, line accepted")
        h.expectSuccess(#"{"state":"armed","volume":0.5,"axis":3,"error":null}"#,
                        HudMessage(state: .armed, volume: 0.5, error: nil, axis: nil),
                        "non-string axis -> nil, line accepted")

        // Extra unknown fields ignored; lenient missing error key
        h.expectSuccess(#"{"state":"idle","volume":0.5,"error":null,"extra":123,"note":"x"}"#,
                        HudMessage(state: .idle, volume: 0.5, error: nil), "unknown extra fields ignored")
        h.expectSuccess(#"{"state":"idle","volume":0.5}"#,
                        HudMessage(state: .idle, volume: 0.5, error: nil), "missing error key defaults to nil")

        // Rejections
        h.expectFailure(#"{"state":"paused","volume":0.5,"error":null}"#,
                        .unknownState("paused"), "unknown state rejected")
        h.expectFailure(#"{"volume":0.5,"error":null}"#, .missingState, "missing state rejected")
        h.expectFailure(#"{"state":"idle","error":null}"#, .missingVolume, "missing volume rejected")
        h.expectFailure(#"{"state":42,"volume":0.5,"error":null}"#, .stateNotString, "numeric state rejected")
        h.expectFailure(#"{"state":"idle","volume":"loud","error":null}"#, .volumeNotNumber, "string volume rejected")
        h.expectFailure(#"{"state":"idle","volume":true,"error":null}"#, .volumeNotNumber, "boolean volume rejected")
        h.expectFailure(#"{"state":"idle","volume":0.5,"error":42}"#, .errorNotStringOrNull, "numeric error field rejected")
        h.expectFailureMatching("not json at all", { if case .notJSON = $0 { return true }; return false },
                                "non-JSON garbage rejected")
        h.expectFailureMatching(#"[1,2,3]"#, { $0 == .notObject }, "top-level array rejected")
        h.expectFailure("", .emptyLine, "empty line rejected")
        h.expectFailure("    \t  ", .emptyLine, "whitespace-only line rejected")
    }

    // MARK: - Visibility reducer

    static func visibilityTests(_ h: Harness) {
        // Helper: extract the single scheduleHide token from a command list.
        func hideToken(_ cmds: [VisibilityCommand]) -> Int? {
            for c in cmds { if case .scheduleHide(_, let t) = c { return t } }
            return nil
        }
        func fadeToken(_ cmds: [VisibilityCommand]) -> Int? {
            for c in cmds { if case .fadeOut(let t) = c { return t } }
            return nil
        }
        func countFadeOut(_ all: [[VisibilityCommand]]) -> Int {
            all.reduce(0) { acc, cmds in acc + cmds.filter { if case .fadeOut = $0 { return true }; return false }.count }
        }

        // V1: 30 idle/s for 3s after armed -> exactly one fadeOut, ~1s after first idle.
        do {
            var m = VisibilityModel(linger: 1.0)
            h.assertEqual(m.advance(.message(state: .armed, now: 0)), [.fadeIn], "V1 armed shows")
            var all: [[VisibilityCommand]] = []
            var fadeNow: Double? = nil
            let step = 1.0 / 30.0
            for i in 0...90 {                       // 0.0 ... 3.0 s
                let now = Double(i) * step
                let cmds = m.advance(.message(state: .idle, now: now))
                all.append(cmds)
                if fadeToken(cmds) != nil, fadeNow == nil { fadeNow = now }
            }
            h.assertEqual(countFadeOut(all), 1, "V1 exactly one fadeOut under the idle flood")
            h.assertTrue(fadeNow != nil && abs(fadeNow! - 1.0) < 0.05,
                         "V1 fadeOut ~1s after first idle", "fired at \(String(describing: fadeNow))")
            h.assertEqual(m.phase, .fadingOut, "V1 ends in fadingOut")
        }

        // V1b: the scheduled-timer path also produces the fade (no now-based net).
        do {
            var m = VisibilityModel(linger: 1.0)
            _ = m.advance(.message(state: .armed, now: 0))
            let idle = m.advance(.message(state: .idle, now: 0))
            guard let t = hideToken(idle) else { h.fail("V1b scheduleHide emitted", "no token"); return }
            h.ok("V1b idle transition schedules hide")
            let fired = m.advance(.hideTimerFired(token: t))
            h.assertTrue(fadeToken(fired) != nil, "V1b timer fired -> fadeOut")
            // A stale timer token does nothing.
            h.assertEqual(m.advance(.hideTimerFired(token: 999)), [], "V1b stale timer token ignored")
        }

        // V2: arming during the fade-out -> ends visible at full alpha.
        do {
            var m = VisibilityModel(linger: 1.0)
            _ = m.advance(.message(state: .armed, now: 0))
            let idle = m.advance(.message(state: .idle, now: 0))
            let t1 = hideToken(idle)!
            let fade = m.advance(.hideTimerFired(token: t1))
            let t2 = fadeToken(fade)!
            let arming = m.advance(.message(state: .arming, now: 0.1))
            h.assertEqual(arming, [.fadeIn, .startPulse], "V2 arming during fade -> fadeIn + pulse")
            h.assertEqual(m.phase, .shown, "V2 phase is shown after re-arm")
            // The cancelled fade's completion must not hide us.
            h.assertEqual(m.advance(.fadeFinished(token: t2)), [], "V2 stale fadeFinished ignored")
            h.assertEqual(m.phase, .shown, "V2 stays shown after stale fadeFinished")
        }

        // V3: armed -> idle -> armed within the linger -> never hides.
        do {
            var m = VisibilityModel(linger: 1.0)
            _ = m.advance(.message(state: .armed, now: 0))
            let idle = m.advance(.message(state: .idle, now: 0.0))
            let t1 = hideToken(idle)!
            let rearm = m.advance(.message(state: .armed, now: 0.5))
            h.assertEqual(rearm, [.cancelFade], "V3 re-arm in linger cancels the pending hide")
            h.assertEqual(m.phase, .shown, "V3 phase shown after re-arm")
            h.assertEqual(m.advance(.hideTimerFired(token: t1)), [], "V3 stale hide timer does nothing")
            h.assertEqual(m.phase, .shown, "V3 never hides")
        }

        // V4: repeated identical messages produce no repeated commands.
        do {
            var m = VisibilityModel(linger: 1.0)
            h.assertEqual(m.advance(.message(state: .armed, now: 0)), [.fadeIn], "V4 first armed -> fadeIn")
            h.assertEqual(m.advance(.message(state: .armed, now: 0.03)), [], "V4 repeat armed -> none")
            h.assertEqual(m.advance(.message(state: .armed, now: 0.06)), [], "V4 repeat armed -> none (2)")
            let idle = m.advance(.message(state: .idle, now: 0.1))
            h.assertTrue(hideToken(idle) != nil, "V4 idle transition schedules hide")
            h.assertEqual(m.advance(.message(state: .idle, now: 0.13)), [], "V4 repeat idle -> none")
            h.assertEqual(m.advance(.message(state: .idle, now: 0.16)), [], "V4 repeat idle -> none (2)")
            let arming = m.advance(.message(state: .arming, now: 0.2))
            h.assertEqual(arming, [.cancelFade, .startPulse], "V4 arming after idle -> cancelFade + pulse")
            h.assertEqual(m.advance(.message(state: .arming, now: 0.23)), [], "V4 repeat arming -> none")
        }

        // V5: hidden while idle (initial idle flood before any gesture).
        do {
            var m = VisibilityModel(linger: 1.0)
            h.assertEqual(m.advance(.message(state: .idle, now: 0)), [], "V5 idle from hidden -> none")
            h.assertEqual(m.advance(.message(state: .idle, now: 0.5)), [], "V5 idle stays hidden")
            h.assertEqual(m.phase, .hidden, "V5 phase remains hidden")
        }

        // V6: pulse toggles only on change (armed<->arming while shown).
        do {
            var m = VisibilityModel(linger: 1.0)
            _ = m.advance(.message(state: .armed, now: 0))            // shown, no pulse
            h.assertEqual(m.advance(.message(state: .arming, now: 0.03)), [.startPulse], "V6 armed->arming starts pulse")
            h.assertEqual(m.advance(.message(state: .armed, now: 0.06)), [.stopPulse], "V6 arming->armed stops pulse")
        }
    }
}
