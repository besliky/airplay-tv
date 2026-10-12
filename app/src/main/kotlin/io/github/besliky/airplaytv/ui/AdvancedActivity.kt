package io.github.besliky.airplaytv.ui

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.Settings as SystemSettings
import android.view.View
import io.github.besliky.airplaytv.BuildConfig
import io.github.besliky.airplaytv.Diagnostics
import io.github.besliky.airplaytv.Identity
import io.github.besliky.airplaytv.PairedDevices
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.core.DecoderSelector
import io.github.besliky.airplaytv.service.ReceiverService
import io.github.besliky.airplaytv.update.Updater

/** Advanced settings and maintenance actions. */
class AdvancedActivity : SettingsPage() {

    private lateinit var settings: Settings
    private lateinit var paired: PairedDevices
    private lateinit var decoderRow: Row
    private lateinit var resolutionRow: Row
    private lateinit var fpsRow: Row
    private lateinit var overlayRow: Row
    private lateinit var logsRow: Row
    private lateinit var pairedRow: Row
    private lateinit var updateRow: Row
    private var available: Updater.Release? = null
    private var waitingForPermission: Updater.Release? = null
    private val installProblems: (Updater.Problem) -> Unit = { showProblem(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        paired = PairedDevices(this)

        titleView.text = getString(R.string.advanced_title)
        statusRow.visibility = View.GONE
        nameView.visibility = View.GONE
        addressView.visibility = View.GONE
        hintView.visibility = View.GONE

        decoderRow = addRow(getString(R.string.setting_decoder)) { chooseDecoder() }.navigates(true)
        resolutionRow = addRow(getString(R.string.setting_resolution)) { cycleResolution() }
        fpsRow = addRow(getString(R.string.setting_frame_rate)) {
            settings.frameRate = if (settings.frameRate == 60) 30 else 60
            bind()
        }
        overlayRow = addRow(getString(R.string.setting_overlay)) {
            settings.performanceOverlay = !settings.performanceOverlay
            bind()
        }
        logsRow = addRow(getString(R.string.setting_logs)) {
            settings.verboseLogging = !settings.verboseLogging
            bind()
        }
        addRow(getString(R.string.setting_diagnostics)) { exportDiagnostics() }.navigates(true)
        pairedRow = addRow(getString(R.string.setting_paired_devices)) { showPairedDevices() }.navigates(true)
        addRow(getString(R.string.setting_reset_pairing)) { resetPairing() }
        addRow(getString(R.string.setting_licenses)) {
            Dialogs.longText(this, getString(R.string.setting_licenses), Diagnostics.licenses(this))
        }.navigates(true)
        updateRow = addRow(getString(R.string.setting_update)) { checkForUpdates() }

        bind()
        rows.getChildAt(0)?.requestFocus()
    }

    private fun bind() {
        decoderRow.value(settings.preferredDecoder.ifEmpty { getString(R.string.value_automatic) })
        resolutionRow.value(settings.resolution.key)
        fpsRow.value(getString(R.string.value_fps, settings.frameRate))
        overlayRow.value(onOff(settings.performanceOverlay))
        logsRow.value(onOff(settings.verboseLogging))
        pairedRow.value(paired.count.toString())
        updateRow.value(available?.let { getString(R.string.value_update_available, it.version.name) } ?: BuildConfig.VERSION_NAME)
    }

    override fun onStart() {
        super.onStart()
        Updater.onInstallProblem = installProblems
        // back from allowing app installs: carry on with the update
        val release = waitingForPermission ?: return
        waitingForPermission = null
        if (packageManager.canRequestPackageInstalls()) download(release)
    }

    override fun onStop() {
        if (Updater.onInstallProblem === installProblems) Updater.onInstallProblem = null
        super.onStop()
    }

    // ---- updates ----

    private fun checkForUpdates() {
        var cancelled = false
        val dialog = Dialogs.progress(this, getString(R.string.setting_update), getString(R.string.update_checking)) {
            cancelled = true
        }
        Updater.check { result ->
            if (cancelled || isDestroyed) return@check
            dialog.dismiss()
            when (result) {
                is Updater.CheckResult.UpToDate -> {
                    available = null
                    Dialogs.message(this, getString(R.string.setting_update), getString(R.string.update_up_to_date, BuildConfig.VERSION_NAME))
                }
                is Updater.CheckResult.Available -> {
                    available = result.release
                    offer(result.release)
                }
                is Updater.CheckResult.Failed -> showProblem(result.problem)
            }
            bind()
        }
    }

    private fun offer(release: Updater.Release) {
        Dialogs.confirm(
            this, getString(R.string.update_available_title, release.version.name), release.notes,
            R.string.update_install,
        ) { install(release) }
    }

    private fun install(release: Updater.Release) {
        if (packageManager.canRequestPackageInstalls()) {
            download(release)
            return
        }
        // The user allows this app to install apps once. Where the TV has no screen for that, the
        // system installer asks by itself when it gets the update.
        Dialogs.confirm(
            this, getString(R.string.update_allow_title), getString(R.string.update_allow_message), R.string.update_allow_open,
        ) {
            try {
                startActivity(Intent(SystemSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
                waitingForPermission = release
            } catch (_: ActivityNotFoundException) {
                download(release)
            }
        }
    }

    private fun download(release: Updater.Release) {
        val title = getString(R.string.update_available_title, release.version.name)
        val dialog = Dialogs.progress(this, title, getString(R.string.update_downloading, 0)) { Updater.cancel() }
        Updater.install(
            this, release,
            progress = { percent ->
                dialog.setMessage(
                    if (percent < 100) getString(R.string.update_downloading, percent) else getString(R.string.update_verifying),
                )
            },
        ) { problem ->
            if (isDestroyed) return@install
            dialog.dismiss()
            problem?.let(::showProblem)
        }
    }

    private fun showProblem(problem: Updater.Problem) {
        if (isDestroyed) return
        Dialogs.message(this, getString(R.string.update_failed_title), getString(problem.message, problem.detail))
    }

    private fun chooseDecoder() {
        val decoders = DecoderSelector.candidates(DecoderSelector.AVC)
        val usable = decoders.filter { it.hardware }.ifEmpty { decoders }
        val names = listOf(getString(R.string.value_automatic)) + usable.map { d ->
            d.name + if (d.lowLatency) "  ·  low latency" else ""
        }
        val selected = usable.indexOfFirst { it.name == settings.preferredDecoder }.let { if (it < 0) 0 else it + 1 }
        Dialogs.choose(this, R.string.dialog_decoder_title, names, selected) { which ->
            settings.preferredDecoder = if (which == 0) "" else usable[which - 1].name
            bind()
        }
    }

    private fun cycleResolution() {
        val next = when (settings.resolution) {
            Settings.Resolution.HD -> Settings.Resolution.FULL_HD
            Settings.Resolution.FULL_HD -> Settings.Resolution.UHD
            Settings.Resolution.UHD -> Settings.Resolution.HD
        }
        if (next == Settings.Resolution.UHD && !DecoderSelector.select(null).hevc4k) {
            settings.resolution = Settings.Resolution.HD
            Dialogs.message(this, getString(R.string.setting_resolution), getString(R.string.value_4k_unavailable))
        } else {
            settings.resolution = next
        }
        bind()
    }

    private fun showPairedDevices() {
        val devices = paired.all()
        val text = if (devices.isEmpty()) "—" else devices.joinToString("\n") { it.name.ifEmpty { "iPhone" } }
        Dialogs.message(this, getString(R.string.setting_paired_devices), text)
    }

    private fun resetPairing() {
        Dialogs.confirm(this, R.string.dialog_reset_title, R.string.dialog_reset_message, R.string.dialog_reset_confirm) {
            paired.clear()
            Identity.resetPairingKey(this)
            ReceiverService.restart(this)
            bind()
            Dialogs.message(this, getString(R.string.setting_reset_pairing), getString(R.string.dialog_reset_done))
        }
    }

    private fun exportDiagnostics() {
        val report = Diagnostics.build(this)
        val file = Diagnostics.save(this, report)
        val header = if (file != null) getString(R.string.dialog_diagnostics_saved, file.absolutePath) + "\n\n" else ""
        Dialogs.longText(this, getString(R.string.dialog_diagnostics_title), header + report)
    }
}
