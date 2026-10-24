package com.rexglue.renut;

import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

import org.libsdl.app.SDLActivity;

// SDL's activity running librenut.so; the manager and the game are native.
public class RenutActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // SDL is linked into librenut.so; librexruntime.so is the SDK's runtime.
        return new String[] { "c++_shared", "rexruntime", "renut" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (!hasStorageAccess())
            requestStorageAccess();
        // The back gesture opens reNut's side menu (Android 13+ sends it here).
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, RenutActivity::backPressed);
        }
    }

    private static native void nativeBackPressed();

    private static void backPressed() {
        try {
            nativeBackPressed();
        } catch (UnsatisfiedLinkError e) {
            // librenut.so not loaded yet
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled())
                backPressed();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        backPressed();
    }

    // Opens the system's "all files access" page for this app.
    public void requestStorageAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // Called from native code (the manager) after a driver change: the
    // Vulkan driver is loaded once per process, so start a new one.
    public void restartApp() {
        Intent intent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        Runtime.getRuntime().exit(0);
    }

    // Whether the game folder on shared storage is readable yet.
    public boolean hasStorageAccess() {
        return Environment.isExternalStorageManager();
    }

    // --- helpers for the manager's workshop and saves (native code calls these
    // from worker threads) ---

    // The body of a GET (redirects followed), or null.
    public static byte[] httpGet(String url) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestProperty("User-Agent", "reNut-Android/1.0");
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            if (c.getResponseCode() != 200)
                return null;
            try (InputStream in = new BufferedInputStream(c.getInputStream())) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; )
                    out.write(buf, 0, n);
                return out.toByteArray();
            }
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null)
                c.disconnect();
        }
    }

    // A PNG or JPEG scaled down to at most maxWidth: {width, height, ARGB...}, or null.
    public static int[] decodeImage(byte[] data, int maxWidth) {
        Bitmap b = BitmapFactory.decodeByteArray(data, 0, data.length);
        if (b == null)
            return null;
        if (b.getWidth() > maxWidth) {
            Bitmap s = Bitmap.createScaledBitmap(b, maxWidth,
                    Math.max(1, b.getHeight() * maxWidth / b.getWidth()), true);
            b.recycle();
            b = s;
        }
        int w = b.getWidth(), h = b.getHeight();
        int[] out = new int[2 + w * h];
        out[0] = w;
        out[1] = h;
        b.getPixels(out, 2, w, 0, 0, w, h);
        b.recycle();
        return out;
    }

    public static String sha256(byte[] data) {
        try {
            StringBuilder hex = new StringBuilder();
            for (byte x : MessageDigest.getInstance("SHA-256").digest(data))
                hex.append(String.format("%02x", x));
            return hex.toString();
        } catch (Exception e) {
            return "";
        }
    }

    // Zips the folder's contents (paths relative to it). Empty, or the error.
    public static String zipFolder(String folder, String zip) {
        File root = new File(folder);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(zip))) {
            addToZip(out, root, "");
            return "";
        } catch (Exception e) {
            new File(zip).delete();
            return String.valueOf(e.getMessage());
        }
    }

    private static void addToZip(ZipOutputStream out, File dir, String prefix) throws Exception {
        File[] files = dir.listFiles();
        if (files == null)
            return;
        byte[] buf = new byte[65536];
        for (File f : files) {
            if (f.isDirectory()) {
                addToZip(out, f, prefix + f.getName() + "/");
                continue;
            }
            out.putNextEntry(new ZipEntry(prefix + f.getName()));
            try (InputStream in = new FileInputStream(f)) {
                for (int n; (n = in.read(buf)) > 0; )
                    out.write(buf, 0, n);
            }
            out.closeEntry();
        }
    }

    // Unpacks a zip into the folder, refusing entries that would land outside
    // it. Empty, or the error.
    public static String unzip(String zip, String folder) {
        try (ZipInputStream in = new ZipInputStream(new FileInputStream(zip))) {
            File root = new File(folder).getCanonicalFile();
            byte[] buf = new byte[65536];
            for (ZipEntry e; (e = in.getNextEntry()) != null; ) {
                File f = new File(root, e.getName()).getCanonicalFile();
                if (!f.getPath().startsWith(root.getPath() + File.separator))
                    return "the zip has a file outside its folder: " + e.getName();
                if (e.isDirectory()) {
                    f.mkdirs();
                    continue;
                }
                f.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(f)) {
                    for (int n; (n = in.read(buf)) > 0; )
                        out.write(buf, 0, n);
                }
            }
            return "";
        } catch (Exception e) {
            return String.valueOf(e.getMessage());
        }
    }
}
