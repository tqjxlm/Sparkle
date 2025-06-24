package io.tqjxlm.sparkle

import android.annotation.SuppressLint
import android.os.Build.VERSION
import android.os.Build.VERSION_CODES
import android.os.Bundle
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager.LayoutParams
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.androidgamesdk.GameActivity

class VulkanActivity : GameActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hideSystemUI()
    }

    private fun hideSystemUI() {
        @SuppressLint("ObsoleteSdkInt")
        if (VERSION.SDK_INT >= VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
        val decorView: View = window.decorView
        val controller = WindowInsetsControllerCompat(
            window,
            decorView
        )
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.hide(WindowInsetsCompat.Type.displayCutout())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        var processed = super.onKeyDown(keyCode, event);
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            onBackPressed()
            processed = true
        }
        return processed
    }

    companion object {
        init {
            System.loadLibrary("sparkle")
        }
    }
}