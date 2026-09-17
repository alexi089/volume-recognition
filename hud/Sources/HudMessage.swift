// HudMessage.swift
//
// The stdin line contract for volume-hud, plus a strict parser/validator.
//
// Depends on Foundation ONLY (no AppKit): it is the display program's data
// boundary and is exercised by build/test_message.
//
// Wire format: one JSON object per line, e.g.
//   {"state":"idle","volume":0.50,"error":null}
//   {"state":"armed","volume":0.62,"axis":"x","error":null}
//   {"state":"armed","volume":0.62,"error":"the output ... did not apply it"}
//
// Behaviours worth knowing (verified against this SDK's JSONSerialization):
//   - Duplicate keys: the FIRST occurrence wins.
//   - A syntactic number that overflows Double may parse to +/-inf (e.g. -1e400)
//     or be rejected outright (e.g. 1e400 throws). Either way a non-finite
//     volume is treated as malformed and the line is rejected.
//   - A huge but finite integer (e.g. 1e23) is accepted and clamped to 1.0.

import Foundation

/// The three display states the HUD can show.
enum HudState: String, Equatable, Sendable {
    case idle
    case arming
    case armed
}

/// Which way the current slide runs. `nil` (absent, null, or unrecognised)
/// means the direction is not yet decided.
enum HudAxis: String, Equatable, Sendable {
    case x
    case y
}

/// One fully-validated line ready for the UI. `volume` is always in 0...1
/// (finite out-of-range inputs are clamped). `error` is nil when there is no
/// error (absent, null, or all-whitespace are normalised to nil). `axis` is nil
/// until a direction is known.
struct HudMessage: Equatable, Sendable {
    var state: HudState
    var volume: Double
    var error: String?
    var axis: HudAxis?

    init(state: HudState, volume: Double, error: String?, axis: HudAxis? = nil) {
        self.state = state
        self.volume = volume
        self.error = error
        self.axis = axis
    }
}

/// Why a line was rejected. Rejected lines are ignored by the reader (with one
/// diagnostic to stderr); they never crash the program.
enum HudParseError: Error, Equatable, Sendable {
    case emptyLine
    case notJSON(String)          // syntactically invalid JSON
    case notObject                // valid JSON, but the top level is not an object
    case missingState
    case stateNotString
    case unknownState(String)
    case missingVolume
    case volumeNotNumber          // not a number, a boolean, or non-finite
    case errorNotStringOrNull

    /// A short, human-readable reason for the stderr diagnostic line.
    var diagnostic: String {
        switch self {
        case .emptyLine:            return "empty line"
        case .notJSON(let m):       return "not valid JSON: \(m)"
        case .notObject:            return "top-level JSON value is not an object"
        case .missingState:         return "missing required field \"state\""
        case .stateNotString:       return "field \"state\" is not a string"
        case .unknownState(let s):  return "unknown state \"\(s)\" (expected idle|arming|armed)"
        case .missingVolume:        return "missing required field \"volume\""
        case .volumeNotNumber:      return "field \"volume\" is not a finite number"
        case .errorNotStringOrNull: return "field \"error\" is not a string or null"
        }
    }
}

enum HudMessageParser {

    /// Parse and validate a single line. Never throws; returns a Result so the
    /// caller can react to each rejection reason without a catch.
    ///
    /// Contract:
    ///  - `state`  : required, one of "idle" | "arming" | "armed".
    ///  - `volume` : required, a finite JSON number; clamped into 0...1
    ///               (booleans and non-finite values rejected).
    ///  - `error`  : optional; JSON null or string. Absent, null, or a string
    ///               that is empty after trimming -> nil.
    ///  - `axis`   : optional; "x" | "y". Absent, null, or anything else -> nil,
    ///               and the line is NOT rejected for a bad axis.
    ///  - Unknown extra fields are ignored; duplicate keys -> first wins.
    static func parse(_ rawLine: String) -> Result<HudMessage, HudParseError> {
        let trimmed = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            return .failure(.emptyLine)
        }

        guard let data = trimmed.data(using: .utf8) else {
            return .failure(.notJSON("could not encode line as UTF-8"))
        }

        let json: Any
        do {
            // Default options: the top level must be an object or array.
            json = try JSONSerialization.jsonObject(with: data)
        } catch {
            return .failure(.notJSON(error.localizedDescription))
        }

        guard let dict = json as? [String: Any] else {
            return .failure(.notObject)
        }

        // state ---------------------------------------------------------------
        guard let stateRaw = dict["state"] else {
            return .failure(.missingState)
        }
        guard let stateString = stateRaw as? String else {
            return .failure(.stateNotString)
        }
        guard let state = HudState(rawValue: stateString) else {
            return .failure(.unknownState(stateString))
        }

        // volume --------------------------------------------------------------
        guard let volumeRaw = dict["volume"] else {
            return .failure(.missingVolume)
        }
        // A JSON bool bridges to NSNumber, so reject it explicitly; a JSON
        // string/null will not cast to NSNumber at all; and a non-finite value
        // (overflow such as -1e400 -> -inf) is not a usable volume.
        guard let volumeNumber = volumeRaw as? NSNumber,
              !isBoolean(volumeNumber),
              volumeNumber.doubleValue.isFinite else {
            return .failure(.volumeNotNumber)
        }
        let volume = clamp01(volumeNumber.doubleValue)

        // error ---------------------------------------------------------------
        var errorText: String? = nil
        if let errorRaw = dict["error"] {          // absent key -> stays nil
            if errorRaw is NSNull {
                errorText = nil
            } else if let s = errorRaw as? String {
                let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
                errorText = t.isEmpty ? nil : t
            } else {
                return .failure(.errorNotStringOrNull)
            }
        }

        // axis (lenient: never a reason to reject the line) -------------------
        var axis: HudAxis? = nil
        if let axisRaw = dict["axis"], !(axisRaw is NSNull), let s = axisRaw as? String {
            axis = HudAxis(rawValue: s)            // unrecognised string -> nil
        }

        return .success(HudMessage(state: state, volume: volume, error: errorText, axis: axis))
    }

    /// Clamp a FINITE value into 0...1. Non-finite inputs are rejected upstream,
    /// so no NaN/inf handling is needed here.
    private static func clamp01(_ v: Double) -> Double {
        min(max(v, 0), 1)
    }

    /// True for a JSON boolean (which toll-free-bridges to NSNumber/CFBoolean).
    private static func isBoolean(_ n: NSNumber) -> Bool {
        CFGetTypeID(n) == CFBooleanGetTypeID()
    }
}
