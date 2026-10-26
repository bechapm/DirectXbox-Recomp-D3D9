package org.xboxrecomp.app;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;

import org.libsdl.app.SDLActivity;

/*
 * The game: SDL's activity, with the engine and the title in one library.
 * SDL is linked statically into libmain.so, so there is no libSDL3.so to load
 * first; SDLActivity calls SDL_main in it, which is src/host/android_main.c.
 *
 * SetupActivity says where the game is (the extra "game_dir"); it reaches the
 * engine as RECOMP_GAME_DIR, set before SDL_main runs. Started without one
 * (adb, during development) the engine looks in the app's own folder.
 *
 * Back opens the in-game menu (GameMenu). Back from a controller is left to
 * the game, whose pad has its own Back button.
 */
public class RecompActivity extends SDLActivity implements GameMenu.Host {
    // src/host/android_main.c
    static native void nativeMenu(boolean open);
    static native String nativeSettings();
    static native void nativeSet(String key, String value);

    private GameMenu menu;

    @Override
    protected void onCreate(Bundle state) {
        String dir = getIntent() != null ? getIntent().getStringExtra(SetupActivity.KEY_GAME) : null;
        if (dir != null) {
            try {
                Os.setenv("RECOMP_GAME_DIR", dir, true);
            } catch (ErrnoException e) {
                Log.e("xboxrecomp", "cannot pass the game folder: " + e);
            }
        }
        super.onCreate(state);
        menu = new GameMenu(this, this);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    private static boolean fromPad(KeyEvent e) {
        int s = e.getSource();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (menu != null && e.getKeyCode() == KeyEvent.KEYCODE_BACK && !fromPad(e)) {
            if (e.getAction() == KeyEvent.ACTION_UP)
                menu.toggle();
            return true;
        }
        if (menu != null && menu.isOpen())
            return menu.onKey(e);
        return super.dispatchKeyEvent(e);
    }

    @Override
    public void onBackPressed() {
        // The gesture, on systems that send it this way rather than as a key.
        if (menu != null)
            menu.toggle();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Coming back with the menu open: it still holds the title.
        if (menu != null && menu.isOpen())
            nativeMenu(true);
    }

    // ---- GameMenu.Host ----

    @Override public String settings() { return nativeSettings(); }
    @Override public void set(String key, String value) { nativeSet(key, value); }
    @Override
    public void opened(boolean open) {
        // The game's surface holds the focus, and takes the d-pad with it, so
        // it cannot have the focus while the menu is open.
        if (mSurface != null) {
            mSurface.setFocusable(!open);
            mSurface.setFocusableInTouchMode(!open);
            if (!open)
                mSurface.requestFocus();
        }
        nativeMenu(open);
    }

    @Override
    public void quit() {
        // Closing the game window on a desktop ends the process; this is the
        // same. The title is held at a frame boundary while the menu is open.
        finishAndRemoveTask();
        new Handler(Looper.getMainLooper()).postDelayed(
            () -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
    }
}
