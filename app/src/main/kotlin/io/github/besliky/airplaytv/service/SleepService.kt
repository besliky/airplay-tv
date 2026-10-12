package io.github.besliky.airplaytv.service

import android.accessibilityservice.AccessibilityService
import android.content.Intent
import android.os.Build
import android.view.accessibility.AccessibilityEvent

/**
 * Puts the TV back to sleep after the music, which Android lets an app do only as an accessibility
 * service (streaming sticks protect their own sleep action with a system permission). It reads
 * nothing from the screen and receives no events; it only offers [sleep], which locks the screen,
 * and on a TV device that means standby, with the TV following over HDMI-CEC.
 *
 * It stays off until the user enables it once, see the Sleep After Music setting.
 */
class SleepService : AccessibilityService() {

    override fun onServiceConnected() {
        super.onServiceConnected()
        instance = this
    }

    override fun onUnbind(intent: Intent?): Boolean {
        instance = null
        return super.onUnbind(intent)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit

    override fun onInterrupt() = Unit

    companion object {
        @Volatile
        private var instance: SleepService? = null

        /** True while the user has the service switched on (locking the screen needs Android 9). */
        val available: Boolean get() = instance != null && Build.VERSION.SDK_INT >= 28

        fun sleep(): Boolean {
            val service = instance ?: return false
            return Build.VERSION.SDK_INT >= 28 && service.performGlobalAction(GLOBAL_ACTION_LOCK_SCREEN)
        }
    }
}
