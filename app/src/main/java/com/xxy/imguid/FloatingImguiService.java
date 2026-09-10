package com.xxy.imguid;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Build;
import android.os.IBinder;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

import android.util.Log;

/**
 * Renders a Dear ImGui window inside a TYPE_APPLICATION_OVERLAY window.
 *
 * The window is a normal (non focusable) overlay so it floats above every other
 * app / the system UI. Moving and resizing is done by mutating
 * {@link WindowManager.LayoutParams} (x, y, width, height) and calling
 */
public class FloatingImguiService extends Service {

    private static final String CHANNEL_ID = "imgui_overlay";
    private static final int NOTIFICATION_ID = 1001;

    private WindowManager windowManager;
    private View overlayRoot;
    private SurfaceView surfaceView;

    /** Lives as long as the window: this is the object we keep mutating. */
    private WindowManager.LayoutParams params;

    private int screenWidth;
    private int screenHeight;
    private float density = 1f;

    @Override
    public void onCreate() {
        super.onCreate();

        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        density = getResources().getDisplayMetrics().density;
        refreshScreenMetrics();

        // Read the bundled TrueType CJK font in Java (reliable) and hand the raw
        // bytes to native. This avoids system-font parsing issues on some ROMs.
        try (InputStream is = getAssets().open("cjk_font.ttf"))
        {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] tmp = new byte[8192];
            int n;
            while ((n = is.read(tmp)) > 0)
                bos.write(tmp, 0, n);
            ImguiBridge.nativeProvideCjkFont(bos.toByteArray());
        }
        catch (IOException e)
        {
            Log.w("ImguiBridge", "bundled CJK font unavailable", e);
        }

        ImguiBridge.nativeInit(getFilesDir().getAbsolutePath(), density, getAssets());

        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());

        addOverlayWindow();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        refreshScreenMetrics();
        if (params != null && overlayRoot != null && overlayRoot.isAttachedToWindow()) {
            params.x = clampX(params.x);
            params.y = clampY(params.y);
            params.width = clampWidth(params.width);
            params.height = clampHeight(params.height);
            windowManager.updateViewLayout(overlayRoot, params);
        }
    }

    @Override
    public void onDestroy() {
        removeOverlayWindow();
        ImguiBridge.nativeDestroy();
        super.onDestroy();
    }

    // ---------------------------------------------------------------- window

    private void addOverlayWindow() {
        overlayRoot = LayoutInflater.from(this).inflate(R.layout.overlay_imgui, null);

        // Translucent surface: ImGui clears with alpha=0 so the overlay floats
        // over whatever is behind it instead of drawing a black rectangle.
        surfaceView = overlayRoot.findViewById(R.id.overlay_surface);
        surfaceView.getHolder().setFormat(PixelFormat.RGBA_8888);

        int width = clampWidth(Math.min(screenWidth - dp(24), dp(520)));
        int height = clampHeight(Math.min(screenHeight - dp(140), dp(680)));

        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE       // never steal focus
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL        // touches outside pass through
                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS       // allow negative x/y
                | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED;

        params = new WindowManager.LayoutParams(
                width,
                height,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                flags,
                PixelFormat.RGBA_8888);
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = (screenWidth - width) / 2;
        params.y = dp(56);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            params.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        windowManager.addView(overlayRoot, params);

        bindViews();
    }

    private void removeOverlayWindow() {
        if (overlayRoot != null) {
            try {
                if (overlayRoot.isAttachedToWindow()) {
                    windowManager.removeView(overlayRoot);
                }
            } catch (Exception e) {
                // Window already gone - nothing to do.
            }
            overlayRoot = null;
            surfaceView = null;
            params = null;
        }
    }

    private void bindViews() {
        surfaceView = overlayRoot.findViewById(R.id.overlay_surface);
        final View titleBar = overlayRoot.findViewById(R.id.overlay_title_bar);
        final View resizeGrip = overlayRoot.findViewById(R.id.overlay_resize_grip);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                ImguiBridge.nativeSurfaceCreated(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                ImguiBridge.nativeSurfaceChanged(holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                ImguiBridge.nativeSurfaceDestroyed();
            }
        });

        // Everything that is not consumed by the drag handles goes to ImGui.
        surfaceView.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    ImguiBridge.nativeTouch(ImguiBridge.ACTION_DOWN, event.getX(), event.getY());
                    return true;
                case MotionEvent.ACTION_MOVE:
                    ImguiBridge.nativeTouch(ImguiBridge.ACTION_MOVE, event.getX(), event.getY());
                    return true;
                case MotionEvent.ACTION_UP:
                    ImguiBridge.nativeTouch(ImguiBridge.ACTION_UP, event.getX(), event.getY());
                    v.performClick();
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    ImguiBridge.nativeTouch(ImguiBridge.ACTION_CANCEL, event.getX(), event.getY());
                    return true;
                default:
                    return false;
            }
        });

        setupMoveByDrag(titleBar);
        setupResizeByDrag(resizeGrip);

        overlayRoot.findViewById(R.id.overlay_close).setOnClickListener(v -> stopSelf());
    }

    // ------------------------------------------------------------ move / resize

    /** Drag anywhere on the title bar -> mutate params.x / params.y. */
    private void setupMoveByDrag(View handle) {
        final int[] start = new int[2];
        final float[] down = new float[2];

        handle.setOnTouchListener((v, event) -> {
            if (params == null || !overlayRoot.isAttachedToWindow()) {
                return false;
            }
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    down[0] = event.getRawX();
                    down[1] = event.getRawY();
                    start[0] = params.x;
                    start[1] = params.y;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    params.x = clampX(start[0] + Math.round(event.getRawX() - down[0]));
                    params.y = clampY(start[1] + Math.round(event.getRawY() - down[1]));
                    windowManager.updateViewLayout(overlayRoot, params);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    v.performClick();
                    return true;
                default:
                    return false;
            }
        });
    }

    /** Drag the bottom-right grip -> mutate params.width / params.height. */
    private void setupResizeByDrag(View handle) {
        final int[] start = new int[2];
        final float[] down = new float[2];

        handle.setOnTouchListener((v, event) -> {
            if (params == null || !overlayRoot.isAttachedToWindow()) {
                return false;
            }
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    down[0] = event.getRawX();
                    down[1] = event.getRawY();
                    start[0] = params.width;
                    start[1] = params.height;
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    int newWidth = clampWidth(start[0] + Math.round(event.getRawX() - down[0]));
                    int newHeight = clampHeight(start[1] + Math.round(event.getRawY() - down[1]));
                    // Threshold avoids re-creating the EGL window surface on every pixel.
                    if (Math.abs(newWidth - params.width) >= 4 || Math.abs(newHeight - params.height) >= 4) {
                        params.width = newWidth;
                        params.height = newHeight;
                        windowManager.updateViewLayout(overlayRoot, params);
                    }
                    return true;
                }
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    v.performClick();
                    return true;
                default:
                    return false;
            }
        });
    }

    private void refreshScreenMetrics() {
        Rect bounds = windowManager.getCurrentWindowMetrics().getBounds();
        screenWidth = bounds.width();
        screenHeight = bounds.height();
    }

    /** Keep at least 48dp of the window on screen so it can always be grabbed again. */
    private int clampX(int x) {
        int min = -(params.width - dp(48));
        int max = screenWidth - dp(48);
        return Math.max(min, Math.min(x, max));
    }

    private int clampY(int y) {
        int min = -(params.height - dp(48));
        int max = screenHeight - dp(48);
        return Math.max(min, Math.min(y, max));
    }

    private int clampWidth(int w) {
        return Math.max(dp(220), Math.min(w, screenWidth));
    }

    private int clampHeight(int h) {
        return Math.max(dp(180), Math.min(h, screenHeight));
    }

    private int dp(float value) {
        return (int) (value * density + 0.5f);
    }

    // ------------------------------------------------------------ notification

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, getString(R.string.notification_channel_name),
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription(getString(R.string.notification_channel_desc));
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification() {
        Intent intent = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(
                this, 0, intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        return new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_overlay)
                .setContentTitle(getString(R.string.notification_title))
                .setContentText(getString(R.string.notification_text))
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
    }
}
