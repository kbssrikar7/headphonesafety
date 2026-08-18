package com.headphonesafety.android

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.os.Build
import android.util.Log
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.sqrt

private const val TAG = "HPS-Harness"

/**
 * Debug-only measurement tap — NOT part of the shipped Headphone Safety feature, and not wired
 * into any release-facing UI. Repurposes `AudioPlaybackCaptureConfiguration`, the same mechanism
 * docs/android-port.md already ruled out as a *limiter* architecture (it's non-destructive, the
 * original audio keeps playing unmodified alongside the captured copy — useless for actually
 * limiting anything) — that non-destructiveness is irrelevant here, since this only needs to
 * measure the real output level, not replace it. Exists to answer one question empirically: does
 * a `DynamicsProcessing` parameter change (postGain, MBC, PreEq) that reads back as requested
 * actually change the audio, or is it silently stored-but-ignored the way a naive read-back check
 * could never tell apart? See the -2dB threshold finding in docs/android-port.md for why read-back
 * alone was never trusted as sufficient proof on this hardware.
 */
object PlaybackCaptureHarness {

    data class Result(val peakDb: Float, val rmsDb: Float, val sampleCount: Int)

    /** Captures USAGE_MEDIA output for [durationMs] and reports peak/RMS level in dBFS.
     * [onResult] is invoked on a background thread with null if capture failed outright. */
    fun capture(mediaProjection: MediaProjection, durationMs: Long, onResult: (Result?) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Log.w(TAG, "AudioPlaybackCaptureConfiguration needs API 29+")
            onResult(null)
            return
        }
        Thread({ runCapture(mediaProjection, durationMs, onResult) }, "hps-harness-capture").start()
    }

    private fun runCapture(
        mediaProjection: MediaProjection,
        durationMs: Long,
        onResult: (Result?) -> Unit
    ) {
        val sampleRate = 48000
        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .build()
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
            .build()
        val minBufferSize = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )

        val record = runCatching {
            AudioRecord.Builder()
                .setAudioFormat(format)
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setBufferSizeInBytes(minBufferSize * 4)
                .build()
        }.onFailure { Log.e(TAG, "AudioRecord.Builder failed", it) }.getOrNull()

        if (record == null || record.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord not initialized (state=${record?.state})")
            onResult(null)
            return
        }

        val buffer = ShortArray(minBufferSize)
        var peak = 0
        var sumSquares = 0.0
        var count = 0

        runCatching {
            record.startRecording()
            val endAt = System.currentTimeMillis() + durationMs
            while (System.currentTimeMillis() < endAt) {
                val read = record.read(buffer, 0, buffer.size)
                for (i in 0 until read) {
                    val sample = buffer[i].toInt()
                    val magnitude = abs(sample)
                    if (magnitude > peak) peak = magnitude
                    sumSquares += sample.toDouble() * sample.toDouble()
                    count++
                }
            }
        }.onFailure { Log.e(TAG, "capture loop failed", it) }

        record.stop()
        record.release()

        if (count == 0) {
            Log.w(TAG, "captured 0 samples — no audio reaching the tap, or capture never started")
            onResult(null)
            return
        }

        val fullScale = 32767.0
        val rms = sqrt(sumSquares / count)
        val peakDb = if (peak > 0) (20 * log10(peak / fullScale)).toFloat() else -96.0f
        val rmsDb = if (rms > 0) (20 * log10(rms / fullScale)).toFloat() else -96.0f
        Log.d(TAG, "capture done: samples=$count peakDb=%.2f rmsDb=%.2f".format(peakDb, rmsDb))
        onResult(Result(peakDb, rmsDb, count))
    }
}
