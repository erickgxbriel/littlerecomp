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
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.libsdl.app.SDLActivity;

public class ScarfaceActivity extends SDLActivity {
    private static final int REQ_PICK_ISO = 1001;
    private static final int REQ_PICK_FOLDER = 1002;
    private static final int REQ_EXPORT_SAVES = 1003;
    private static final int REQ_IMPORT_SAVES = 1004;
    private static final int REQ_IMPORT_MOD = 1005;
    private static volatile ScarfaceActivity sInstance = null;

    private static native void nativeOnIsoPicked(String uri, String displayName);
    private static native void nativeOnFolderPicked(String path);
    private static native void nativeOnSavesResult(boolean ok, String message);
    private static native void nativeOnModImportResult(boolean ok, String message);

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

    public void exportSavesZip() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/zip");
                intent.putExtra(Intent.EXTRA_TITLE, "LittleRecomp_Saves.zip");
                startActivityForResult(intent, REQ_EXPORT_SAVES);
            } catch (Exception e) {
                try {
                    nativeOnSavesResult(false, "Cannot open file creator: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        });
    }

    public void importSavesZip() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                    "application/zip", "application/x-zip-compressed", "application/octet-stream"
                });
                startActivityForResult(intent, REQ_IMPORT_SAVES);
            } catch (Exception e) {
                try {
                    nativeOnSavesResult(false, "Cannot open file picker: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        });
    }

    public void importModZip() {
        runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                    "application/zip", "application/x-zip-compressed", "application/octet-stream"
                });
                startActivityForResult(intent, REQ_IMPORT_MOD);
            } catch (Exception e) {
                try {
                    nativeOnModImportResult(false, "Cannot open file picker: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        });
    }

    private File getSavesTitleDir() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        return new File(base, "hdd/E/UDATA/5655003f");
    }

    private int zipDirRecursive(File root, File dir, ZipOutputStream zos, byte[] buf) throws Exception {
        File[] list = dir.listFiles();
        if (list == null) return 0;
        int count = 0;
        String rootPath = root.getAbsolutePath();
        for (File f : list) {
            if (f.isDirectory()) {
                count += zipDirRecursive(root, f, zos, buf);
            } else if (f.isFile()) {
                String full = f.getAbsolutePath();
                String rel = full.startsWith(rootPath + File.separator)
                    ? full.substring(rootPath.length() + 1)
                    : f.getName();
                rel = rel.replace(File.separatorChar, '/');
                zos.putNextEntry(new ZipEntry(rel));
                try (FileInputStream fis = new FileInputStream(f)) {
                    int n;
                    while ((n = fis.read(buf)) > 0) {
                        zos.write(buf, 0, n);
                    }
                }
                zos.closeEntry();
                count++;
            }
        }
        return count;
    }

    private void runExportSavesAsync(Uri uri) {
        new Thread(() -> {
            File savesDir = getSavesTitleDir();
            if (!savesDir.exists() || !savesDir.isDirectory()) {
                try {
                    nativeOnSavesResult(false, "No save folder found");
                } catch (UnsatisfiedLinkError ignored) {
                }
                return;
            }
            try (OutputStream os = getContentResolver().openOutputStream(uri, "wt")) {
                if (os == null) {
                    nativeOnSavesResult(false, "Failed to open output file");
                    return;
                }
                int count;
                try (ZipOutputStream zos = new ZipOutputStream(new BufferedOutputStream(os))) {
                    byte[] buf = new byte[16384];
                    count = zipDirRecursive(savesDir, savesDir, zos, buf);
                }
                if (count == 0) {
                    nativeOnSavesResult(false, "No save files found to export");
                } else {
                    nativeOnSavesResult(true, "Backup ZIP saved (" + count + " file(s))");
                }
            } catch (Exception e) {
                try {
                    nativeOnSavesResult(false, "Export failed: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        }, "save_export").start();
    }

    private void ensureWritableSavesDir(File base, File savesDir) {
        if (!savesDir.exists()) {
            savesDir.mkdirs();
        }
        File probe = new File(savesDir, ".write_probe");
        try (FileOutputStream fos = new FileOutputStream(probe)) {
            fos.write(1);
            probe.delete();
            return;
        } catch (Exception ignored) {
            if (probe.exists()) probe.delete();
        }
        // Recover from Android 11+ FUSE ownership mismatch (e.g. directory created by adb shell)
        File hdd = new File(base, "hdd");
        if (hdd.exists()) {
            File backup = new File(base, "hdd.bak_" + System.currentTimeMillis());
            hdd.renameTo(backup);
        }
        savesDir.mkdirs();
    }

    private void runImportSavesAsync(Uri uri) {
        new Thread(() -> {
            File base = getExternalFilesDir(null);
            if (base == null) base = getFilesDir();
            File savesDir = getSavesTitleDir();
            ensureWritableSavesDir(base, savesDir);
            int count = 0;
            try {
                String rootCanon = savesDir.getCanonicalPath() + File.separator;
                try (InputStream is = getContentResolver().openInputStream(uri)) {
                    if (is == null) {
                        nativeOnSavesResult(false, "Failed to open ZIP file");
                        return;
                    }
                    try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(is))) {
                        byte[] buf = new byte[16384];
                        ZipEntry entry;
                        while ((entry = zis.getNextEntry()) != null) {
                            if (entry.isDirectory()) {
                                zis.closeEntry();
                                continue;
                            }
                            String name = entry.getName();
                            if (name == null) {
                                zis.closeEntry();
                                continue;
                            }
                            name = name.replace('\\', '/');
                            while (name.startsWith("/")) name = name.substring(1);
                            int marker = name.toLowerCase(Locale.US).indexOf("5655003f/");
                            String rel = marker >= 0 ? name.substring(marker + "5655003f/".length()) : name;
                            if (rel.isEmpty() || rel.startsWith("__MACOSX/")) {
                                zis.closeEntry();
                                continue;
                            }
                            File target = new File(savesDir, rel);
                            String targetCanon = target.getCanonicalPath();
                            if (!targetCanon.startsWith(rootCanon)) {
                                zis.closeEntry();
                                continue;
                            }
                            File parent = target.getParentFile();
                            if (parent != null && !parent.exists()) {
                                parent.mkdirs();
                            }
                            if (target.exists()) {
                                target.delete();
                            }
                            long written = 0;
                            try (FileOutputStream fos = new FileOutputStream(target)) {
                                int n;
                                while ((n = zis.read(buf)) > 0) {
                                    written += n;
                                    if (written > 64L * 1024L * 1024L) {
                                        throw new IllegalStateException("Save entry too large");
                                    }
                                    fos.write(buf, 0, n);
                                }
                            }
                            count++;
                            zis.closeEntry();
                        }
                    }
                }
                if (count == 0) {
                    nativeOnSavesResult(false, "No valid save files found inside ZIP");
                } else {
                    nativeOnSavesResult(true, "Imported " + count + " file(s) cleanly");
                }
            } catch (Exception e) {
                try {
                    nativeOnSavesResult(false, "Import failed: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        }, "save_import").start();
    }

    private void runImportModAsync(Uri uri) {
        new Thread(() -> {
            int count = 0;
            try {
                File base = getExternalFilesDir(null);
                if (base == null) base = getFilesDir();
                File modsDir = new File(base, "mods");
                if (!modsDir.exists()) modsDir.mkdirs();
                String rootCanon = modsDir.getCanonicalPath() + File.separator;

                try (InputStream is = getContentResolver().openInputStream(uri)) {
                    if (is == null) {
                        nativeOnModImportResult(false, "Failed to open mod ZIP");
                        return;
                    }
                    try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(is))) {
                        byte[] buf = new byte[16384];
                        ZipEntry entry;
                        while ((entry = zis.getNextEntry()) != null) {
                            if (entry.isDirectory()) {
                                zis.closeEntry();
                                continue;
                            }
                            String name = entry.getName();
                            if (name == null) {
                                zis.closeEntry();
                                continue;
                            }
                            name = name.replace('\\', '/');
                            while (name.startsWith("/")) name = name.substring(1);
                            if (name.toLowerCase(Locale.US).startsWith("mods/")) {
                                name = name.substring(5);
                            }
                            if (name.isEmpty() || name.startsWith("__MACOSX/")) {
                                zis.closeEntry();
                                continue;
                            }
                            // Root-level .tsv packs go into mods/ptbr/ automatically
                            String rel = (!name.contains("/") && name.toLowerCase(Locale.US).endsWith(".tsv"))
                                ? ("ptbr/" + name)
                                : name;
                            File target = new File(modsDir, rel);
                            String targetCanon = target.getCanonicalPath();
                            if (!targetCanon.startsWith(rootCanon)) {
                                zis.closeEntry();
                                continue;
                            }
                            File parent = target.getParentFile();
                            if (parent != null && !parent.exists()) {
                                parent.mkdirs();
                            }
                            long written = 0;
                            try (FileOutputStream fos = new FileOutputStream(target)) {
                                int n;
                                while ((n = zis.read(buf)) > 0) {
                                    written += n;
                                    if (written > 256L * 1024L * 1024L) {
                                        throw new IllegalStateException("Mod entry too large");
                                    }
                                    fos.write(buf, 0, n);
                                }
                            }
                            count++;
                            zis.closeEntry();
                        }
                    }
                }
                if (count == 0) {
                    nativeOnModImportResult(false, "No valid mod/translation files inside ZIP");
                } else {
                    nativeOnModImportResult(true, "Imported " + count + " mod file(s) into mods/");
                }
            } catch (Exception e) {
                try {
                    nativeOnModImportResult(false, "Mod import failed: " + e.getMessage());
                } catch (UnsatisfiedLinkError ignored) {
                }
            }
        }, "mod_import").start();
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
        if (requestCode == REQ_PICK_ISO || requestCode == REQ_PICK_FOLDER) {
            int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            if ((takeFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0) {
                takeFlags |= Intent.FLAG_GRANT_READ_URI_PERMISSION;
            }
            try {
                ContentResolver cr = getContentResolver();
                cr.takePersistableUriPermission(uri, takeFlags & Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception ignored) {
            }
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
        } else if (requestCode == REQ_EXPORT_SAVES) {
            runExportSavesAsync(uri);
        } else if (requestCode == REQ_IMPORT_SAVES) {
            runImportSavesAsync(uri);
        } else if (requestCode == REQ_IMPORT_MOD) {
            runImportModAsync(uri);
        }
    }
}
