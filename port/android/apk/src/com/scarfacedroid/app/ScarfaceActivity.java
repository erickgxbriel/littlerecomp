package com.scarfacedroid.app;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.Cursor;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.util.Locale;
import org.libsdl.app.SDLActivity;

public class ScarfaceActivity extends SDLActivity {
    private static final int REQ_PICK_ISO = 1001;
    private static final int REQ_PICK_FOLDER = 1002;
    private static volatile ScarfaceActivity sInstance = null;

    private static native void nativeOnIsoPicked(String uri, String displayName);
    private static native void nativeOnFolderPicked(String path);

    @Override
    protected String[] getLibraries() {
        // SDL3 first, then main (which pulls DT_NEEDED libScarface.so so libmain.so interposes weak symbols)
        return new String[] {
            "SDL3",
            "main",
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 28 && getWindow() != null) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode =
                Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        applyImmersive();
    }

    @Override
    protected void onDestroy() {
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersive();
        }
    }

    @SuppressWarnings("deprecation")
    private void applyImmersive() {
        if (getWindow() == null || getWindow().getDecorView() == null) return;
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        );
    }

    public void openIsoPicker() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(intent, REQ_PICK_ISO);
            } catch (Exception ignored) {
            }
        });
    }

    public void openFolderPicker() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(intent, REQ_PICK_FOLDER);
            } catch (Exception ignored) {
            }
        });
    }

    public boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT < 30) return true;
        return Environment.isExternalStorageManager();
    }

    public void requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT < 30) return;
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception e) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception ignored) {
                }
            }
        });
    }

    public int openPersistedUriFd(String uriStr) {
        if (uriStr == null || uriStr.isEmpty()) return -1;
        try {
            Uri uri = Uri.parse(uriStr);
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) return -1;
            return pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    public String getDisplayNameForUri(String uriStr) {
        if (uriStr == null || uriStr.isEmpty()) return "";
        try {
            Uri uri = Uri.parse(uriStr);
            try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    String name = c.getString(0);
                    if (name != null) return name;
                }
            }
            String last = uri.getLastPathSegment();
            return last != null ? last : uriStr;
        } catch (Exception e) {
            return uriStr;
        }
    }

    public String getSystemLanguage() {
        return Locale.getDefault().getLanguage();
    }

    public float getBatteryTempC() {
        try {
            IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            Intent batteryStatus = registerReceiver(null, filter);
            if (batteryStatus == null) return -1.0f;
            int tenths = batteryStatus.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -1);
            if (tenths <= 0) return -1.0f;
            return tenths / 10.0f;
        } catch (Exception e) {
            return -1.0f;
        }
    }

    private static String decodeTreeUriToPath(Uri treeUri) {
        if (treeUri == null) return "";
        try {
            String docId = DocumentsContract.getTreeDocumentId(treeUri);
            if (docId == null || docId.isEmpty()) return "";
            if (docId.startsWith("raw:")) {
                return docId.substring(4);
            }
            int colon = docId.indexOf(':');
            String vol = colon >= 0 ? docId.substring(0, colon) : docId;
            String rel = colon >= 0 ? docId.substring(colon + 1) : "";
            String base;
            if ("primary".equalsIgnoreCase(vol)) {
                base = Environment.getExternalStorageDirectory().getAbsolutePath();
            } else {
                base = "/storage/" + vol;
            }
            if (rel.isEmpty()) return base;
            return base + File.separator + rel;
        } catch (Exception e) {
            return "";
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if ((takeFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0) {
            takeFlags |= Intent.FLAG_GRANT_READ_URI_PERMISSION;
        }
        try {
            ContentResolver cr = getContentResolver();
            cr.takePersistableUriPermission(uri, takeFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }

        if (requestCode == REQ_PICK_ISO) {
            String uriStr = uri.toString();
            String name = getDisplayNameForUri(uriStr);
            try {
                nativeOnIsoPicked(uriStr, name);
            } catch (UnsatisfiedLinkError ignored) {
            }
        } else if (requestCode == REQ_PICK_FOLDER) {
            String path = decodeTreeUriToPath(uri);
            if (!path.isEmpty()) {
                try {
                    nativeOnFolderPicked(path);
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        }
    }
}
