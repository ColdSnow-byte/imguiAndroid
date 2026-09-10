package com.xxy.imguid;

import android.content.res.AssetManager;
import android.view.Surface;

/** Thin JNI wrapper around the native ImGui renderer. */
final class
ImguiBridge {

    /** Keep in sync with TouchAction in imgui_overlay.cpp */
    static final int ACTION_DOWN = 0;
    static final int ACTION_MOVE = 1;
    static final int ACTION_UP = 2;
    static final int ACTION_CANCEL = 3;

    static {
        System.loadLibrary("imgui_overlay");
    }

    private ImguiBridge() {
    }

    static native void nativeInit(String filesDir, float density, AssetManager assetManager);

    static native void nativeProvideCjkFont(byte[] data);

    static native void nativeDestroy();

    static native void nativeSurfaceCreated(Surface surface);

    static native void nativeSurfaceChanged(Surface surface, int width, int height);

    static native void nativeSurfaceDestroyed();

    static native void nativeTouch(int action, float x, float y);
}
