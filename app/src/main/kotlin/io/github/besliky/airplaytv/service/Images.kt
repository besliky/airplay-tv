// The platform ExifInterface that lint warns about had its problems on Android 7 and older; minSdk is 26.
@file:SuppressLint("ExifInterface")

package io.github.besliky.airplaytv.service

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.os.Handler
import android.os.Looper
import java.io.ByteArrayInputStream
import java.io.IOException
import java.util.concurrent.Executors

/**
 * Decodes covers and photos from the sender on a background thread. Pictures are shrunk while
 * decoding (a 12-megapixel photo would take 48 MB at full size) and turned upright by their EXIF
 * orientation.
 */
object Images {

    /** Longest side of a cover; it is shown at about 400 dp. */
    const val COVER_SIDE = 1024

    /** Longest side of a photo; enough for a 4K screen. */
    const val PHOTO_SIDE = 2560

    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor { task -> Thread(task, "AirPlayImages").apply { isDaemon = true } }

    /** Decodes [data] off the main thread and hands the picture, or null, to [done] on the main thread. */
    fun decode(data: ByteArray, maxSide: Int, done: (Bitmap?) -> Unit) {
        executor.execute {
            val bitmap = decodeNow(data, maxSide)
            main.post { done(bitmap) }
        }
    }

    private fun decodeNow(data: ByteArray, maxSide: Int): Bitmap? = try {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            null
        } else {
            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / sample > maxSide) sample *= 2
            val options = BitmapFactory.Options().apply { inSampleSize = sample }
            BitmapFactory.decodeByteArray(data, 0, data.size, options)?.let { upright(it, orientation(data)) }
        }
    } catch (_: RuntimeException) {
        null
    } catch (_: OutOfMemoryError) {
        null
    }

    private fun orientation(data: ByteArray): Int = try {
        ExifInterface(ByteArrayInputStream(data)).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
    } catch (_: IOException) {
        ExifInterface.ORIENTATION_NORMAL
    }

    private fun upright(bitmap: Bitmap, orientation: Int): Bitmap {
        val matrix = Matrix()
        when (orientation) {
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.postScale(-1f, 1f)
            ExifInterface.ORIENTATION_ROTATE_180 -> matrix.postRotate(180f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> matrix.postScale(1f, -1f)
            ExifInterface.ORIENTATION_TRANSPOSE -> {
                matrix.postRotate(90f)
                matrix.postScale(-1f, 1f)
            }
            ExifInterface.ORIENTATION_ROTATE_90 -> matrix.postRotate(90f)
            ExifInterface.ORIENTATION_TRANSVERSE -> {
                matrix.postRotate(-90f)
                matrix.postScale(-1f, 1f)
            }
            ExifInterface.ORIENTATION_ROTATE_270 -> matrix.postRotate(-90f)
            else -> return bitmap
        }
        val turned = Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
        if (turned !== bitmap) bitmap.recycle()
        return turned
    }
}
