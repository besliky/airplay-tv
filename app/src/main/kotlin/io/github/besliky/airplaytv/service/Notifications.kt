package io.github.besliky.airplaytv.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.ui.Page
import io.github.besliky.airplaytv.ui.Pages

object Notifications {
    const val FOREGROUND_ID = 1
    private const val SESSION_ID = 2
    private const val CHANNEL_SERVICE = "receiver"
    private const val CHANNEL_SESSION = "session"

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java) ?: return
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_SERVICE, context.getString(R.string.channel_receiver), NotificationManager.IMPORTANCE_MIN).apply {
                setShowBadge(false)
                setSound(null, null)
                enableVibration(false)
            }
        )
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_SESSION, context.getString(R.string.channel_session), NotificationManager.IMPORTANCE_HIGH).apply {
                setShowBadge(false)
                setSound(null, null)
                enableVibration(false)
            }
        )
    }

    fun foreground(context: Context, text: String): Notification {
        val open = PendingIntent.getActivity(
            context, 0,
            Pages.intent(context, Page.HOME).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Builder(context, CHANNEL_SERVICE)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.title_airplay))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .build()
    }

    /** Fallback when the system does not let the service open the playback screen itself. */
    fun showSessionPrompt(context: Context, clientName: String) {
        val nm = context.getSystemService(NotificationManager::class.java) ?: return
        val intent = PendingIntent.getActivity(
            context, 1,
            Pages.intent(context, Page.MIRROR).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val n = Notification.Builder(context, CHANNEL_SESSION)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.title_airplay))
            .setContentText(context.getString(R.string.notification_session, clientName))
            .setContentIntent(intent)
            .setFullScreenIntent(intent, true)
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_CALL)
            .build()
        try {
            nm.notify(SESSION_ID, n)
        } catch (_: SecurityException) {
        }
    }

    fun cancelSessionPrompt(context: Context) {
        context.getSystemService(NotificationManager::class.java)?.cancel(SESSION_ID)
    }
}
