package io.github.besliky.airplaytv.ui

import android.app.Activity
import android.app.AlertDialog
import android.text.InputFilter
import android.text.InputType
import android.util.TypedValue
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ScrollView
import android.widget.TextView
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings

/** Small framework dialogs styled for the TV theme. */
object Dialogs {

    private fun builder(activity: Activity) = AlertDialog.Builder(activity, R.style.Theme_AirPlayTV_Dialog)

    private fun dp(activity: Activity, v: Int) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), activity.resources.displayMetrics).toInt()

    fun editName(activity: Activity, current: String, onDone: (String) -> Unit) {
        val input = EditText(activity).apply {
            setText(current)
            setSelection(current.length)
            hint = activity.getString(R.string.dialog_name_hint)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
            imeOptions = EditorInfo.IME_ACTION_DONE
            filters = arrayOf(InputFilter.LengthFilter(Settings.MAX_NAME_LENGTH))
            setSingleLine()
            setTextColor(activity.getColor(R.color.text_primary))
            setHintTextColor(activity.getColor(R.color.text_secondary))
            textSize = 22f
        }
        val container = FrameLayout(activity).apply {
            setPadding(dp(activity, 24), dp(activity, 8), dp(activity, 24), 0)
            addView(input)
        }
        val dialog = builder(activity)
            .setTitle(R.string.dialog_name_title)
            .setView(container)
            .setPositiveButton(R.string.dialog_ok) { _, _ ->
                Settings.sanitizeName(input.text.toString())?.let(onDone)
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .create()
        input.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                Settings.sanitizeName(input.text.toString())?.let(onDone)
                dialog.dismiss()
                true
            } else {
                false
            }
        }
        dialog.show()
        input.requestFocus()
    }

    fun confirm(activity: Activity, title: Int, message: Int, confirmLabel: Int, onConfirm: () -> Unit) {
        builder(activity)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(confirmLabel) { _, _ -> onConfirm() }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    fun confirm(activity: Activity, title: CharSequence, message: CharSequence, confirmLabel: Int, onConfirm: () -> Unit) {
        builder(activity)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(confirmLabel) { _, _ -> onConfirm() }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    /** A message that changes while some work runs. Leaving the dialog calls [onCancel]. */
    fun progress(activity: Activity, title: CharSequence, message: CharSequence, onCancel: () -> Unit): AlertDialog =
        builder(activity)
            .setTitle(title)
            .setMessage(message)
            .setNegativeButton(R.string.dialog_cancel) { _, _ -> onCancel() }
            .setOnCancelListener { onCancel() }
            .show()

    fun message(activity: Activity, title: CharSequence, message: CharSequence) {
        builder(activity)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(R.string.dialog_close, null)
            .show()
    }

    /** Scrollable monospace text, for diagnostics and license texts. */
    fun longText(activity: Activity, title: CharSequence, text: CharSequence) {
        val tv = TextView(activity).apply {
            this.text = text
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 13f
            setTextColor(activity.getColor(R.color.text_primary))
            setPadding(dp(activity, 24), dp(activity, 8), dp(activity, 24), dp(activity, 8))
            setTextIsSelectable(false)
            isFocusable = true
        }
        val scroll = ScrollView(activity).apply { addView(tv) }
        builder(activity)
            .setTitle(title)
            .setView(scroll)
            .setPositiveButton(R.string.dialog_close, null)
            .show()
    }

    fun choose(activity: Activity, title: Int, items: List<String>, selected: Int, onChoose: (Int) -> Unit) {
        builder(activity)
            .setTitle(title)
            .setSingleChoiceItems(items.toTypedArray(), selected) { dialog, which ->
                onChoose(which)
                dialog.dismiss()
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }
}
