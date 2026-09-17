// MessageCoalescer.swift
//
// The producer sends ~28-30 lines per second. Rather than dispatch a separate
// main-thread block per line, the background reader submits each parsed message
// here; the coalescer keeps only the LATEST and schedules a single drain onto
// the main thread. If several lines arrive before the drain runs, only the last
// is applied. This bounds main-thread work to one apply() per runloop turn.

import Foundation

final class MessageCoalescer: @unchecked Sendable {

    private let lock = NSLock()
    private var latest: HudMessage?
    private var scheduled = false
    private let apply: @Sendable (HudMessage) -> Void   // invoked on the main queue

    init(apply: @escaping @Sendable (HudMessage) -> Void) {
        self.apply = apply
    }

    /// Called from the background reader for every parsed message.
    func submit(_ message: HudMessage) {
        lock.lock()
        latest = message
        let needSchedule = !scheduled
        if needSchedule { scheduled = true }
        lock.unlock()

        if needSchedule {
            DispatchQueue.main.async { [weak self] in self?.drain() }
        }
    }

    private func drain() {
        lock.lock()
        let message = latest
        latest = nil
        scheduled = false
        lock.unlock()

        if let message { apply(message) }
    }
}
