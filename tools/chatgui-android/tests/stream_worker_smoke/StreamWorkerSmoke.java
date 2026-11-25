import android.media.ImageReader;
import android.graphics.ImageFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.Surface;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.concurrent.atomic.*;

/** Device smoke test: uses production classes from APK, without installing/stopping the user's app. */
public class StreamWorkerSmoke {
    static Object invoke(Object owner, String name, Object... args) throws Exception {
        for (Method m : owner.getClass().getDeclaredMethods()) {
            if (m.getName().equals(name) && m.getParameterCount() == args.length) {
                m.setAccessible(true);
                try { return m.invoke(owner, args); }
                catch (InvocationTargetException e) { throw new RuntimeException(name, e.getCause()); }
            }
        }
        throw new NoSuchMethodException(name);
    }
    static Class<?> type(String name) throws Exception { return Class.forName("com.utun.chat.data." + name); }
    static Object callback(Runnable body) throws Exception {
        Class<?> function = Class.forName("kotlin.jvm.functions.Function0");
        Object unit = Class.forName("kotlin.Unit").getField("INSTANCE").get(null);
        return Proxy.newProxyInstance(function.getClassLoader(), new Class<?>[]{function}, (p, m, a) -> {
            if (m.getName().equals("invoke")) { body.run(); return unit; }
            if (m.getName().equals("toString")) return "smoke callback";
            return null;
        });
    }
    static long underruns(String text) {
        return Long.parseLong(text.split("underruns=")[1].split(" ")[0]);
    }
    static long played(String text) { return Long.parseLong(text.split("played=")[1].split(" ")[0]); }
    public static void main(String[] args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> { e.printStackTrace(); System.exit(1); });
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        HandlerThread images = new HandlerThread("smoke-images"); images.start();
        ImageReader reader = ImageReader.newInstance(128, 72, ImageFormat.PRIVATE, 4);
        AtomicInteger received = new AtomicInteger();
        reader.setOnImageAvailableListener(r -> {
            try (android.media.Image image = r.acquireLatestImage()) { if (image != null) received.incrementAndGet(); }
        }, new Handler(images.getLooper()));
        Object noop = callback(() -> {});
        Object nativeLib = type("NativeLib").getField("INSTANCE").get(null);
        long opus = (Long)invoke(nativeLib, "streamOpusCreate", true);
        byte[] packet = (byte[])invoke(nativeLib, "nativeStreamOpusEncode", opus, new short[1920], false, 25, 1);
        invoke(nativeLib, "streamOpusClose", opus);
        if (packet == null) throw new AssertionError("Opus encode failed");
        byte[] frame = Files.readAllBytes(Paths.get(args[0]));
        Object codec = type("StreamVideoCodec").getField("H264").get(null);
        Class<?> function = Class.forName("kotlin.jvm.functions.Function0");
        Constructor<?> audioCtor = type("StreamMixedPlayback").getConstructor(long.class, long.class, boolean.class, function);
        Constructor<?> frameCtor = type("StreamVideoPlayback$Frame").getConstructor(long.class, boolean.class,
            int.class, int.class, int.class, long.class, type("StreamVideoCodec"), byte[].class, byte[].class);
        AtomicReference<Object> currentAudio = new AtomicReference<>();
        AtomicInteger shown = new AtomicInteger();
        AtomicBoolean stalled = new AtomicBoolean();
        AtomicReference<Throwable> problem = new AtomicReference<>();
        Object render = callback(() -> {
            if (shown.incrementAndGet() == 15) {
                try {
                    Object audio = currentAudio.get();
                    String before = (String)invoke(audio, "getDescription");
                    System.out.println("STALL start " + before);
                    Thread.sleep(500);
                    String after = (String)invoke(audio, "getDescription");
                    System.out.println("STALL end " + after);
                    if (underruns(after) != underruns(before)) throw new AssertionError("Audio gap during video stall");
                    if (played(after) - played(before) < 20000) throw new AssertionError("Audio clock stalled with video");
                    stalled.set(true);
                } catch (Throwable e) { problem.set(e); }
            }
        });
        Object video = type("StreamVideoPlayback").getConstructor(long.class, Surface.class, long.class, function, function)
            .newInstance(4242L, reader.getSurface(), System.nanoTime() / 1000, noop, render);
        try {
            for (int phase = 0; phase < 2; phase++) {
                if (phase != 0) invoke(video, "reset");
                int frames = phase == 0 ? 120 : 3;
                long duration = frames * 33333L;
                Object audio = audioCtor.newInstance(4242L + phase, 0L, true, noop);
                currentAudio.set(audio);
                long audioPts = 0, position = 0, deadline = System.nanoTime() + 15_000_000_000L;
                int supplied = 0;
                boolean videoFinished = false, audioFinished = false, done = false;
                try {
                    while (System.nanoTime() < deadline) {
                        Object failure = invoke(video, "getFailure");
                        if (failure != null) throw new AssertionError("Video failed", (Throwable)failure);
                        if (problem.get() != null) throw new AssertionError("Stall test failed", problem.get());
                        while (audioPts < duration && (Boolean)invoke(audio, "getNeedsInput")) {
                            int result = (Integer)invoke(audio, "feed", audioPts, packet);
                            if (result < 0) throw new AssertionError("Opus feed=" + result);
                            audioPts += 20000;
                        }
                        if (audioPts >= duration && !audioFinished) { invoke(audio, "finish"); audioFinished = true; }
                        invoke(audio, "prepare");
                        while (supplied < frames && (Boolean)invoke(video, "getWantsInput")) {
                            invoke(video, "offer", frameCtor.newInstance(supplied++ * 33333L, true, 128, 72, 0, 1L,
                                codec, new byte[0], frame));
                        }
                        if (supplied == frames && !videoFinished) { invoke(video, "finish"); videoFinished = true; }
                        Object state = invoke(video, "getState");
                        long lead = (Long)invoke(audio, "getLeadUs");
                        if (invoke(state, "getFirstOutputPts") != null) {
                            long[] clock = (long[])invoke(audio, "update", System.nanoTime() / 1000, audioPts, Math.max(40000L, lead + 20000));
                            if (clock == null) throw new AssertionError("Clock failed");
                            position = clock[0];
                        }
                        invoke(video, "update", position, audioPts, lead);
                        if ((Boolean)invoke(state, "getEnded") && (Boolean)invoke(audio, "getDrained")) {
                            long decoded = (Long)invoke(state, "getDecoded");
                            if (decoded != frames) throw new AssertionError("Lost frames " + state);
                            System.out.println("PASS phase=" + phase + " " + state + " " + invoke(audio, "getDescription"));
                            done = true; break;
                        }
                        Thread.sleep(5);
                    }
                    if (!done) throw new AssertionError("Drain timeout " + invoke(video, "getState") + " " + invoke(audio, "getDescription"));
                } finally { invoke(audio, "close"); }
            }
            if (!stalled.get()) throw new AssertionError("Stall was not exercised");
            System.out.println("PASS 500ms video stall: no new audio gaps; EOF/reset: 120 then 3 decoded; surfaces=" + received.get());
        } finally {
            invoke(video, "close"); reader.close(); images.quitSafely(); images.join();
        }
        System.exit(0);
    }
}
