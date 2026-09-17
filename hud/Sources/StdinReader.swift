// StdinReader.swift
//
// Reads one line per JSON message from standard input on a background queue,
// parses each with HudMessageParser, and hands results back through @Sendable
// callbacks. The caller dispatches UI work to the main thread.
//
// Lifecycle: EOF on stdin (parent closed the pipe, or the parent process died,
// which closes the pipe's write end) ends the loop and fires onEOF exactly once.
// Malformed lines are ignored with a single diagnostic to stderr; empty lines
// are skipped silently.

import Foundation

final class StdinReader {

    private let onMessage: @Sendable (HudMessage) -> Void
    private let onEOF: @Sendable () -> Void
    private let queue = DispatchQueue(label: "com.gesture-slider.volume-hud.stdin")

    init(onMessage: @escaping @Sendable (HudMessage) -> Void,
         onEOF: @escaping @Sendable () -> Void) {
        self.onMessage = onMessage
        self.onEOF = onEOF
    }

    /// Begin reading. Returns immediately; work happens on the background queue.
    func start() {
        // Capture only the @Sendable closures, never self, to satisfy Swift 6.
        queue.async { [onMessage, onEOF] in
            while let line = readLine(strippingNewline: true) {
                switch HudMessageParser.parse(line) {
                case .success(let message):
                    onMessage(message)
                case .failure(.emptyLine):
                    continue                 // benign; skip quietly
                case .failure(let error):
                    let note = "volume-hud: ignoring malformed line: \(error.diagnostic)\n"
                    FileHandle.standardError.write(Data(note.utf8))
                }
            }
            onEOF()
        }
    }
}
