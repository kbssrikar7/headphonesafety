import Foundation

// A small thread-safe circular buffer of interleaved Float32 samples, bridging the
// BlackHole capture callback (producer, runs on its own realtime thread) and the
// playback engine's source node render block (consumer, runs on a different realtime
// thread). Lock-based rather than fully lock-free - simple and correct, not
// pro-audio-plugin-grade, but sufficient for a personal utility at these buffer sizes.
final class RingBuffer {
    private var buffer: [Float]
    private var readIndex = 0
    private var writeIndex = 0
    private var filled = 0
    private let capacity: Int
    private let lock = NSLock()

    init(capacity: Int) {
        self.capacity = capacity
        self.buffer = [Float](repeating: 0, count: capacity)
    }

    func write(_ data: UnsafePointer<Float>, count writeCount: Int) {
        lock.lock()
        defer { lock.unlock() }
        for i in 0..<writeCount {
            buffer[writeIndex] = data[i]
            writeIndex = (writeIndex + 1) % capacity
            if filled < capacity {
                filled += 1
            } else {
                // Buffer full: drop the oldest sample rather than block/grow.
                readIndex = (readIndex + 1) % capacity
            }
        }
    }

    // Fills `out` with up to `readCount` samples; pads with silence if underrun.
    func read(into out: UnsafeMutablePointer<Float>, count readCount: Int) {
        lock.lock()
        defer { lock.unlock() }
        let n = min(readCount, filled)
        for i in 0..<n {
            out[i] = buffer[readIndex]
            readIndex = (readIndex + 1) % capacity
        }
        filled -= n
        if n < readCount {
            for i in n..<readCount { out[i] = 0 }
        }
    }

    func reset() {
        lock.lock()
        defer { lock.unlock() }
        readIndex = 0
        writeIndex = 0
        filled = 0
    }
}
