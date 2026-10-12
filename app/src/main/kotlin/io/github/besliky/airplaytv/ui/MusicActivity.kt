package io.github.besliky.airplaytv.ui

import android.os.Bundle
import android.view.View
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.service.SleepService

/** Settings for music from the iPhone: what the Now Playing screen offers and how the TV behaves. */
class MusicActivity : SettingsPage() {

    private lateinit var settings: Settings
    private lateinit var lyricsRow: Row
    private lateinit var sleepRow: Row
    private lateinit var volumeRow: Row

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)

        titleView.text = getString(R.string.music_title)
        statusRow.visibility = View.GONE
        nameView.visibility = View.GONE
        addressView.visibility = View.GONE
        hintView.text = getString(R.string.music_hint)

        lyricsRow = addRow(getString(R.string.setting_lyrics)) { toggleLyrics() }
        sleepRow = addRow(getString(R.string.setting_sleep)) { cycleSleep() }
        volumeRow = addRow(getString(R.string.setting_remote_volume)) { toggleRemoteVolume() }

        bind()
        rows.getChildAt(0)?.requestFocus()
    }

    override fun onStart() {
        super.onStart()
        bind() // the sleep service may have been switched on meanwhile
    }

    private fun bind() {
        lyricsRow.value(onOff(settings.lyricsEnabled))
        sleepRow.value(
            when {
                settings.sleepAfterMinutes == 0 -> getString(R.string.value_off)
                !SleepService.available -> getString(R.string.value_needs_setup)
                else -> getString(R.string.value_minutes, settings.sleepAfterMinutes)
            },
        )
        volumeRow.value(getString(if (settings.remoteVolumeKeys) R.string.value_airplay else R.string.value_tv))
    }

    private fun toggleLyrics() {
        settings.lyricsEnabled = !settings.lyricsEnabled
        bind()
        if (settings.lyricsEnabled) {
            Dialogs.message(this, getString(R.string.dialog_lyrics_title), getString(R.string.dialog_lyrics_message))
        }
    }

    private fun cycleSleep() {
        val choices = Settings.SLEEP_CHOICES
        val wasOff = settings.sleepAfterMinutes == 0
        settings.sleepAfterMinutes = choices[(choices.indexOf(settings.sleepAfterMinutes) + 1) % choices.size]
        bind()
        if (wasOff && !SleepService.available) {
            val service = "$packageName/${SleepService::class.java.name}"
            Dialogs.message(this, getString(R.string.dialog_sleep_title), getString(R.string.dialog_sleep_message, service))
        }
    }

    private fun toggleRemoteVolume() {
        settings.remoteVolumeKeys = !settings.remoteVolumeKeys
        bind()
        if (settings.remoteVolumeKeys) {
            Dialogs.message(this, getString(R.string.dialog_remote_volume_title), getString(R.string.dialog_remote_volume_message))
        }
    }
}
