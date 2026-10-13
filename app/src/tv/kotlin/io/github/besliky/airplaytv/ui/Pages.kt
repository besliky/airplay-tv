package io.github.besliky.airplaytv.ui

import android.content.Context
import android.content.Intent

/** The TV app's screens for each [Page]. */
object Pages {
    fun intent(context: Context, page: Page): Intent = Intent(
        context,
        when (page) {
            Page.HOME -> MainActivity::class.java
            Page.MIRROR -> MirrorActivity::class.java
            Page.MUSIC -> NowPlayingActivity::class.java
        },
    )
}
