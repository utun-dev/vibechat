package com.utun.audioprobe;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/** Изолированная проба: только тишина на выходе, захват сразу отбрасывается. Основное приложение не вызывается. */
public final class ProbeActivity extends Activity {
    private static final String TAG = "AudioProbe";
    static { System.loadLibrary("call_audio_probe"); }
    private static native boolean openNative(boolean exclusive, int channels, boolean callback);
    private static native int writeNative(short[] input);
    private static native void sampleNative(long[] out);
    private static native void closeNative();
    private volatile boolean cancelled;
    private AudioManager audio;
    private TextView status;
    private final AtomicInteger captureError = new AtomicInteger();
    private final AtomicLong captured = new AtomicLong();

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        status = new TextView(this); status.setTextSize(18); status.setPadding(30, 100, 30, 30); setContentView(status);
        status.setText("uTun audio probe\nSilent output; microphone data discarded.\nResults: adb logcat -s AudioProbe");
        audio = getSystemService(AudioManager.class);
        setVolumeControlStream(AudioManager.STREAM_VOICE_CALL);
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "RECORD_AUDIO permission required; install debug APK with adb install -g");
            status.append("\nMicrophone permission missing."); return;
        }
        new Thread(this::runProbe, "audio-probe-owner").start();
    }

    @Override protected void onStop() { cancelled = true; super.onStop(); }

    private void show(String text) {
        Log.i(TAG, text);
        runOnUiThread(() -> { if (!isDestroyed()) status.setText(text + "\nResults: adb logcat -s AudioProbe"); });
    }

    private void runProbe() {
        int previousMode = audio.getMode();
        if (previousMode != AudioManager.MODE_NORMAL) {
            Log.e(TAG, "probe refused: audio mode=" + previousMode + "; finish other calls first"); return;
        }
        AudioRecord record = null;
        Thread capture = null;
        try {
            audio.setMode(AudioManager.MODE_IN_COMMUNICATION);
            int min = AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0) throw new IllegalStateException("record minimum=" + min);
            record = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, 48000,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(min * 2, 3840));
            if (record.getState() != AudioRecord.STATE_INITIALIZED) throw new IllegalStateException("record not initialized");
            record.startRecording();
            if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) throw new IllegalStateException("record not started");
            AudioRecord input = record;
            capture = new Thread(() -> {
                short[] discard = new short[960];
                while (!cancelled) {
                    int n = input.read(discard, 0, discard.length, AudioRecord.READ_BLOCKING);
                    if (n <= 0) {
                        if (!cancelled) { captureError.set(n == 0 ? -1 : n); Log.e(TAG, "capture read=" + n); }
                        break;
                    }
                    captured.addAndGet(n);
                }
            }, "audio-probe-capture");
            capture.start();
            String api = getIntent().getStringExtra("api");
            int channels = getIntent().getIntExtra("channels", 0);
            if (api == null || api.equals("track")) trial("track-mono", 0, 1);
            if (api == null || api.equals("aaudio")) {
                String sharing = getIntent().getStringExtra("sharing");
                for (int c : new int[]{1, 2}) {
                    if (channels != 0 && channels != c) continue;
                    for (int mode : new int[]{1, 2}) {
                        String name = mode == 1 ? "shared" : "exclusive";
                        if (sharing == null || sharing.equals(name)) trial("aaudio-" + name + "-ch" + c, mode, c);
                    }
                }
            }
            show("probe complete captured=" + captured.get() + " captureError=" + captureError.get() + " cancelled=" + cancelled);
        } catch (Exception e) {
            Log.e(TAG, "probe failed", e); show("probe failed: " + e);
        } finally {
            cancelled = true;
            if (record != null) {
                try { record.stop(); } catch (Exception e) { Log.e(TAG, "capture stop failed", e); }
                if (capture != null) {
                    try { capture.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); Log.e(TAG, "capture join", e); }
                }
                record.release();
            }
            if (Build.VERSION.SDK_INT >= 31) audio.clearCommunicationDevice();
            else audio.setSpeakerphoneOn(false);
            audio.setMode(previousMode);
            Log.i(TAG, "probe resources released mode=" + audio.getMode());
        }
    }

    private void route(int type) throws InterruptedException {
        if (cancelled) return;
        if (Build.VERSION.SDK_INT >= 31) {
            AudioDeviceInfo wanted = null;
            for (AudioDeviceInfo d : audio.getAvailableCommunicationDevices()) if (d.getType() == type) wanted = d;
            if (wanted == null || !audio.setCommunicationDevice(wanted)) throw new IllegalStateException("route unavailable type=" + type);
            long deadline = SystemClock.elapsedRealtime() + 2000;
            while (!cancelled && SystemClock.elapsedRealtime() < deadline) {
                AudioDeviceInfo actual = audio.getCommunicationDevice();
                if (actual != null && actual.getId() == wanted.getId()) {
                    Log.i(TAG, "route confirmed type=" + type + " id=" + actual.getId() + " mode=" + audio.getMode());
                    Thread.sleep(500); return;
                }
                Thread.sleep(50);
            }
            throw new IllegalStateException("route confirmation timeout type=" + type);
        }
        audio.setSpeakerphoneOn(type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
        Thread.sleep(500);
    }

    private interface Output extends AutoCloseable {
        void sample(long[] out);
        void close();
    }

    private Output open(int mode, int channels) {
        if (mode == 0) return new TrackOutput();
        boolean callback = getIntent().getBooleanExtra("callback", true);
        if (!openNative(mode == 2, channels, callback)) throw new IllegalStateException("AAudio open rejected");
        AtomicInteger writeError = new AtomicInteger();
        java.util.concurrent.atomic.AtomicBoolean writing = new java.util.concurrent.atomic.AtomicBoolean(!callback);
        Thread writer = new Thread(() -> {
            short[] silence = new short[960];
            try {
                while (writing.get() && !cancelled) {
                    int n = writeNative(silence);
                    if (n < 0) { writeError.set(n); Log.e(TAG, "AAudio write=" + n); break; }
                    if (n == 0) Thread.sleep(2);
                }
            } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }, "audio-probe-aaudio-write");
        if (!callback) writer.start();
        return new Output() {
            public void sample(long[] out) { sampleNative(out); if (writeError.get() != 0) out[7] = writeError.get(); }
            public void close() {
                writing.set(false);
                if (!callback) {
                    writer.interrupt();
                    try { writer.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); Log.e(TAG, "AAudio join", e); }
                }
                closeNative();
            }
        };
    }

    private void trial(String name, int mode, int channels) throws InterruptedException {
        if (cancelled) return;
        show("trial " + name);
        try {
            route(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
            try (Output output = open(mode, channels)) {
                measure(output, name + "/speaker-new");
                route(AudioDeviceInfo.TYPE_BUILTIN_EARPIECE);
                measure(output, name + "/earpiece-existing");
            }
            if (!cancelled) try (Output output = open(mode, channels)) {
                measure(output, name + "/earpiece-new");
                if (getIntent().getBooleanExtra("roundtrip", false)) {
                    route(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
                    measure(output, name + "/speaker-existing");
                }
            }
            if (!cancelled && getIntent().getBooleanExtra("roundtrip", false)) {
                try (Output output = open(mode, channels)) { measure(output, name + "/speaker-reopened"); }
            }
        } catch (RuntimeException e) { Log.e(TAG, "trial failed name=" + name, e); }
    }

    private void measure(Output output, String label) throws InterruptedException {
        long start = System.nanoTime(), report = start, captureStart = captured.get();
        long duration = Math.max(4, Math.min(30, getIntent().getIntExtra("seconds", 8))) * 1_000_000_000L;
        long[] row = new long[10];
        long low = Long.MAX_VALUE, high = Long.MIN_VALUE, sum = 0;
        int count = 0, invalid = 0;
        long underStart = -1, previousUnder = -1;
        while (!cancelled && System.nanoTime() - start < duration) {
            output.sample(row);
            long now = System.nanoTime();
            if (underStart < 0) underStart = row[5];
            if (previousUnder >= 0 && row[5] > previousUnder) Log.w(TAG, "underrun label=" + label + " delta=" + (row[5] - previousUnder));
            previousUnder = row[5];
            if (row[7] != 0 || captureError.get() != 0) {
                Log.e(TAG, "stream failed label=" + label + " output=" + row[7] + " capture=" + captureError.get()); break;
            }
            long delay = row[4] > 0 ? (row[2] - now) / 1000 + (row[0] - row[1]) * 1_000_000 / row[4] : -1;
            boolean valid = row[3] == 0 && row[1] >= 0 && row[1] <= row[0] && row[2] > 0 &&
                now - row[2] >= -200_000_000 && now - row[2] <= 500_000_000 && delay >= 0 && delay <= 2_000_000;
            if (now - start >= 2_000_000_000L) {
                if (valid) { low = Math.min(low, delay); high = Math.max(high, delay); sum += delay; count++; }
                else invalid++;
            }
            if (now - report >= 1_000_000_000L) {
                Log.i(TAG, "sample label=" + label + " outputUs=" + (valid ? delay : -1) + " written=" + row[0] +
                    " stamp=" + row[1] + "@" + row[2] + " status=" + row[3] + " underruns=" + row[5] +
                    " device=" + row[6] + " callbacks=" + row[8] + " callbackFrames=" + row[9] + " captureFrames=" + captured.get());
                report = now;
            }
            Thread.sleep(50);
        }
        Log.i(TAG, "RESULT label=" + label + " samples=" + count + " invalid=" + invalid + " outputUs=" +
            (count == 0 ? "unknown" : low + "/" + sum / count + "/" + high) + " underrunDelta=" + (row[5] - underStart) +
            " captureFrames=" + (captured.get() - captureStart) + " streamError=" + row[7]);
    }

    private final class TrackOutput implements Output {
        private final AudioTrack track;
        private final Thread writer;
        private final AtomicLong written = new AtomicLong();
        private final AtomicInteger error = new AtomicInteger();
        private final AudioTimestamp stamp = new AudioTimestamp();
        private volatile boolean running = true;
        TrackOutput() {
            int min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0) throw new IllegalStateException("track minimum=" + min);
            track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes(Math.max(min * 2, 3840)).setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build();
            try {
                if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new IllegalStateException("track not initialized");
                int bufferMs = getIntent().getIntExtra("buffer_ms", 20);
                if (bufferMs != 20 && bufferMs != 40) throw new IllegalArgumentException("buffer_ms must be 20 or 40");
                int actual = track.setBufferSizeInFrames(48 * bufferMs);
                if (actual <= 0) throw new IllegalStateException("track buffer result=" + actual);
                if (Build.VERSION.SDK_INT >= 31 && track.setStartThresholdInFrames(actual) <= 0)
                    throw new IllegalStateException("track start threshold rejected");
                Log.i(TAG, "track opened requested=1 performance=" + track.getPerformanceMode() + " buffer=" + actual);
                track.play();
            } catch (RuntimeException e) { track.release(); throw e; }
            writer = new Thread(() -> {
                short[] silence = new short[960];
                while (running && !cancelled) {
                    int n = track.write(silence, 0, silence.length, AudioTrack.WRITE_BLOCKING);
                    if (n <= 0) {
                        if (running && !cancelled) { error.set(n == 0 ? -1 : n); Log.e(TAG, "track write=" + n); }
                        break;
                    }
                    written.addAndGet(n);
                }
            }, "audio-probe-track");
            writer.start();
        }
        public void sample(long[] out) {
            out[0] = written.get();
            boolean ok = track.getTimestamp(stamp);
            out[1] = ok ? stamp.framePosition : -1; out[2] = ok ? stamp.nanoTime : -1; out[3] = ok ? 0 : -1;
            out[4] = 48000; out[5] = track.getUnderrunCount();
            AudioDeviceInfo route = track.getRoutedDevice(); out[6] = route == null ? -1 : route.getId();
            out[7] = error.get(); out[8] = -1; out[9] = 960;
        }
        public void close() {
            running = false; track.stop();
            try { writer.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); Log.e(TAG, "track join", e); }
            track.release();
        }
    }
}
