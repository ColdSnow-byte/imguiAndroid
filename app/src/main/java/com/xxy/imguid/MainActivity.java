package com.xxy.imguid;

import android.Manifest;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

public class MainActivity extends AppCompatActivity {

    private TextView statusText;

    private final ActivityResultLauncher<Intent> overlaySettingsLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(),
                    result -> updateStatus());

    private final ActivityResultLauncher<String> notificationPermissionLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestPermission(),
                    granted -> startOverlay());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusText = findViewById(R.id.status_text);

        // Edge-to-edge is enforced from targetSdk 35, and OEMs differ in whether
        // they reserve space for the system/action bar. Applying the system-bar
        // insets ourselves makes the result identical everywhere: non-edge-to-edge
        // devices report zero insets, edge-to-edge ones get the status bar height.
        View root = findViewById(R.id.main);
        final int padTop = root.getPaddingTop();
        final int padBottom = root.getPaddingBottom();
        ViewCompat.setOnApplyWindowInsetsListener(root, (v, insets) -> {
            Insets bars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(v.getPaddingLeft(), padTop + bars.top,
                    v.getPaddingRight(), padBottom + bars.bottom);
            return insets;
        });

        findViewById(R.id.btn_permission).setOnClickListener(v -> requestOverlayPermission());
        findViewById(R.id.btn_start).setOnClickListener(v -> startOverlay());
        findViewById(R.id.btn_stop).setOnClickListener(v -> stopService(new Intent(this, FloatingImguiService.class)));

        updateStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateStatus();
    }

    private boolean hasOverlayPermission() {
        return Settings.canDrawOverlays(this);
    }

    private void requestOverlayPermission() {
        if (hasOverlayPermission()) {
            Toast.makeText(this, R.string.toast_already_granted, Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        overlaySettingsLauncher.launch(intent);
    }

    private void startOverlay() {
        if (!hasOverlayPermission()) {
            Toast.makeText(this, R.string.toast_need_permission, Toast.LENGTH_SHORT).show();
            requestOverlayPermission();
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS);
            return;
        }

        Intent intent = new Intent(this, FloatingImguiService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
    }

    private void updateStatus() {
        boolean granted = hasOverlayPermission();
        statusText.setText(granted ? R.string.status_ready : R.string.status_denied);
    }
}
