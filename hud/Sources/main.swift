// main.swift
//
// Entry point for volume-hud. Top-level code in main.swift runs on the main
// actor, so the AppKit setup below is main-actor-isolated as required.
//
// The app is an .accessory agent: no Dock icon, no menu bar. It reads JSON
// messages from stdin on a background thread, coalesces bursts, and updates the
// HUD on the main thread. It quits cleanly on EOF (parent closed the pipe or
// died) and on SIGINT / SIGTERM.

import AppKit

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let options = HudOptions.parse(CommandLine.arguments)
for warning in options.warnings {
    FileHandle.standardError.write(Data("volume-hud: \(warning)\n".utf8))
}

let controller = AppController(options: options)
app.delegate = controller

// Coalesce the ~30 msgs/s stream to one apply() per main-thread turn.
// AppController is @MainActor-isolated (hence Sendable) so it may be captured.
let coalescer = MessageCoalescer(apply: { message in
    MainActor.assumeIsolated { controller.apply(message) }
})

let reader = StdinReader(
    onMessage: { message in coalescer.submit(message) },
    onEOF: {
        DispatchQueue.main.async {
            MainActor.assumeIsolated { controller.requestQuit() }
        }
    }
)
reader.start()

// Quit cleanly on SIGINT/SIGTERM. Default disposition is ignored first, then a
// dispatch source on the main queue drives an orderly NSApp.terminate. Without a
// Dock icon a stuck process is invisible, so this matters.
signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
let signalSources: [DispatchSourceSignal] = [SIGINT, SIGTERM].map { sig in
    let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    source.setEventHandler {
        MainActor.assumeIsolated { controller.requestQuit() }
    }
    source.resume()
    return source
}
_ = signalSources   // retain for the process lifetime

app.run()
