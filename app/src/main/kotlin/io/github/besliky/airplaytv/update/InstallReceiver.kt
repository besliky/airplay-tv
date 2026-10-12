package io.github.besliky.airplaytv.update

import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.SERVICE
import io.github.besliky.airplaytv.R

/** The system installer's answer to an update that [Updater] handed over. */
class InstallReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE).orEmpty()
        when (status) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                // The system asks the user to confirm the update (and, the first time, to allow it).
                val confirm = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(Intent.EXTRA_INTENT)
                } ?: return
                try {
                    context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                } catch (e: ActivityNotFoundException) {
                    Log.w(SERVICE, "the installer cannot ask for confirmation", e)
                    Updater.installProblem(Updater.Problem(R.string.update_failed_install, "no installer"))
                }
            }
            PackageInstaller.STATUS_SUCCESS -> Log.i(SERVICE, "update installed")
            PackageInstaller.STATUS_FAILURE_ABORTED -> Log.i(SERVICE, "update cancelled")
            PackageInstaller.STATUS_FAILURE_CONFLICT -> {
                // most often an update signed with another key than the installed app
                Log.w(SERVICE, "update refused: $message")
                Updater.installProblem(Updater.Problem(R.string.update_failed_signature))
            }
            else -> {
                Log.w(SERVICE, "update failed ($status): $message")
                Updater.installProblem(Updater.Problem(R.string.update_failed_install, message.ifEmpty { "$status" }))
            }
        }
    }
}
