package com.utun.audioprobe;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import java.util.concurrent.atomic.AtomicInteger;

/** Настоящий TextureView/EGL, один кадр до ACK. Без камеры, сети и основного приложения.
 * am start -n com.utun.audioprobe/.VideoSurfaceProbeActivity --ei fps 60 --ez legacy false
 * legacy регистрирует commit во время draw; fixed — в preDraw, как видеозвонок.
 * Постороннее UI-обновление каждые 500мс воспроизводит зависимость от статистики звонка. */
public final class VideoSurfaceProbeActivity extends Activity implements TextureView.SurfaceTextureListener, ViewTreeObserver.OnPreDrawListener {
    private static final String TAG = "VideoSurfaceProbe";
    private final Handler main = new Handler();
    private final HandlerThread producer = new HandlerThread("video-surface-probe");
    private Handler render;
    private TextureView view;
    private ViewTreeObserver observer;
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;
    private Surface surface;
    private SurfaceTexture texture;
    private volatile boolean cancelled;
    private volatile long expectedPts = -1, submittedAt;
    private final AtomicInteger submitted = new AtomicInteger();
    private int acknowledged, updates, staleChecks, lastReported;
    private int fps;
    private boolean legacy;
    private long started, ackMaxUs;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        fps = getIntent().getIntExtra("fps", 60); legacy = getIntent().getBooleanExtra("legacy", false);
        if (fps != 30 && fps != 60) { Log.e(TAG, "Unsupported fps=" + fps); finish(); return; }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        producer.start(); render = new Handler(producer.getLooper());
        view = new TextureView(this); view.setSurfaceTextureListener(this); setContentView(view);
        Log.i(TAG, "start legacy=" + legacy + " fps=" + fps);
    }

    @Override public void onSurfaceTextureAvailable(SurfaceTexture next, int width, int height) {
        Log.i(TAG, "surface available hardware=" + view.isHardwareAccelerated() + " view=" + width + "x" + height);
        texture = next; next.setDefaultBufferSize(640, 360); surface = new Surface(next);
        observer = view.getViewTreeObserver(); observer.addOnPreDrawListener(this);
        render.post(() -> {
            try {
                display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] version = new int[2]; check(EGL14.eglInitialize(display, version, 0, version, 1), "initialize");
                int[] attributes = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                    EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_NONE};
                EGLConfig[] config = new EGLConfig[1]; int[] count = new int[1];
                check(EGL14.eglChooseConfig(display, attributes, 0, config, 0, 1, count, 0) && count[0] == 1, "config");
                context = EGL14.eglCreateContext(display, config[0], EGL14.EGL_NO_CONTEXT,
                    new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                check(context != EGL14.EGL_NO_CONTEXT, "context");
                window = EGL14.eglCreateWindowSurface(display, config[0], surface, new int[]{EGL14.EGL_NONE}, 0);
                check(window != EGL14.EGL_NO_SURFACE && EGL14.eglMakeCurrent(display, window, window, context), "surface");
                started = System.nanoTime(); main.post(this::tick); send();
            } catch (Exception e) { Log.e(TAG, "producer failed", e); main.post(this::finish); }
        });
    }

    private void send() {
        if (cancelled) return;
        try {
            int number = submitted.get(); long pts = 1_000_000L + number * 1_000_000L / fps;
            GLES20.glClearColor((number % 3) / 2f, ((number + 1) % 3) / 2f, ((number + 2) % 3) / 2f, 1);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            expectedPts = pts; submittedAt = System.nanoTime(); submitted.incrementAndGet();
            check(EGLExt.eglPresentationTimeANDROID(display, window, pts * 1000), "timestamp");
            check(EGL14.eglSwapBuffers(display, window), "swap");
        } catch (Exception e) { Log.e(TAG, "frame failed", e); main.post(this::finish); }
    }

    private void committed() {
        if (cancelled || texture == null || expectedPts < 0) return;
        long pts = texture.getTimestamp() / 1000;
        // Посторонний UI draw может завершиться до прихода следующего буфера. Это не ACK:
        // ожидаем TextureView update, который сам создаст следующий draw/commit.
        if (pts != expectedPts) {
            if (++staleChecks == 1) Log.d(TAG, "defer ACK timestamp=" + pts + " expected=" + expectedPts);
            return;
        }
        expectedPts = -1; acknowledged++;
        ackMaxUs = Math.max(ackMaxUs, (System.nanoTime() - submittedAt) / 1000);
        long next = started + submitted.get() * 1_000_000_000L / fps;
        render.postDelayed(this::send, Math.max(0, (next - System.nanoTime() + 999_999) / 1_000_000));
    }

    @Override public boolean onPreDraw() {
        if (!cancelled && !legacy) view.getViewTreeObserver().registerFrameCommitCallback(this::committed);
        return true;
    }
    @Override public void onSurfaceTextureUpdated(SurfaceTexture source) {
        updates++;
        if (legacy) view.getViewTreeObserver().registerFrameCommitCallback(this::committed);
    }
    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture source, int width, int height) {}
    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture source) {
        if (observer != null && observer.isAlive()) observer.removeOnPreDrawListener(this);
        observer = null; cancelled = true;
        render.post(() -> {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window);
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
            if (display != EGL14.EGL_NO_DISPLAY) EGL14.eglTerminate(display);
            surface.release(); source.release(); producer.quitSafely();
        });
        return false;
    }

    private void tick() {
        if (cancelled) return;
        long elapsed = System.nanoTime() - started;
        Log.i(TAG, "progress legacy=" + legacy + " fps=" + fps + " elapsedMs=" + elapsed / 1_000_000 + " acknowledged=" + acknowledged
            + " advanced=" + (acknowledged - lastReported) + " updates=" + updates + " ackMaxUs=" + ackMaxUs);
        lastReported = acknowledged;
        if (elapsed >= 8_000_000_000L) {
            boolean valid = legacy ? acknowledged < 30 : acknowledged >= fps * 8 * 0.8
                && acknowledged <= submitted.get() && submitted.get() - acknowledged <= 1;
            Log.i(TAG, "RESULT " + (valid ? "PASS" : "FAIL") + " legacy=" + legacy + " targetFps=" + fps + " submitted=" + submitted.get()
                + " acknowledged=" + acknowledged + " updates=" + updates + " staleChecks=" + staleChecks + " ackMaxUs=" + ackMaxUs);
            finish(); return;
        }
        view.invalidate(); main.postDelayed(this::tick, 500);
    }
    @Override protected void onStop() {
        cancelled = true;
        if (texture == null) { Log.i(TAG, "stopped before surface became available"); producer.quitSafely(); }
        super.onStop();
    }
    private static void check(boolean condition, String stage) {
        if (!condition) throw new IllegalStateException(stage + " EGL error=" + EGL14.eglGetError());
    }
}
