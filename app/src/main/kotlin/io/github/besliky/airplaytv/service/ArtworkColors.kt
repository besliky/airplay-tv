package io.github.besliky.airplaytv.service

import android.graphics.Bitmap
import android.graphics.Color

/**
 * Colours taken from a cover: a vivid [accent] for the bars and two dark shades for the backdrop.
 * The shades stay dark (HSV value 0.07 to 0.22), which is easy on OLED panels.
 */
data class ArtworkColors(val accent: Int, val backdropTop: Int, val backdropBottom: Int) {

    companion object {
        /** For no cover, or one too dark or too grey to have a colour. */
        val NEUTRAL = ArtworkColors(0xFFA6A6AC.toInt(), 0xFF17171A.toInt(), 0xFF0B0B0D.toInt())

        private const val SAMPLE = 40
        private const val HUE_BUCKETS = 24

        /** Below this much coloured area (out of SAMPLE² pixels) a cover counts as colourless. */
        private const val MIN_WEIGHT = 12f

        /** The dominant hue of [bitmap], weighted by saturation and brightness. */
        fun from(bitmap: Bitmap): ArtworkColors {
            if (bitmap.width <= 0 || bitmap.height <= 0) return NEUTRAL
            val small = try {
                Bitmap.createScaledBitmap(bitmap, SAMPLE, SAMPLE, true)
            } catch (_: RuntimeException) {
                return NEUTRAL
            }
            val pixels = IntArray(SAMPLE * SAMPLE)
            small.getPixels(pixels, 0, SAMPLE, 0, 0, SAMPLE, SAMPLE)
            if (small !== bitmap) small.recycle()

            val weight = FloatArray(HUE_BUCKETS)
            val hueSum = FloatArray(HUE_BUCKETS)
            val satSum = FloatArray(HUE_BUCKETS)
            val valSum = FloatArray(HUE_BUCKETS)
            val hsv = FloatArray(3)
            var total = 0f
            for (pixel in pixels) {
                if (Color.alpha(pixel) < 200) continue
                Color.colorToHSV(pixel, hsv)
                val (hue, sat, value) = Triple(hsv[0], hsv[1], hsv[2])
                if (value < 0.18f || sat < 0.18f) continue // near black or grey: no colour to take
                val w = sat * (0.4f + 0.6f * value)
                val bucket = (hue / 360f * HUE_BUCKETS).toInt().coerceIn(0, HUE_BUCKETS - 1)
                weight[bucket] += w
                hueSum[bucket] += hue * w
                satSum[bucket] += sat * w
                valSum[bucket] += value * w
                total += w
            }
            if (total < MIN_WEIGHT) return NEUTRAL

            val best = weight.indices.maxBy { weight[it] }
            val hue = hueSum[best] / weight[best]
            val sat = satSum[best] / weight[best]
            val value = valSum[best] / weight[best]
            return ArtworkColors(
                accent = Color.HSVToColor(floatArrayOf(hue, sat.coerceIn(0.35f, 0.85f), value.coerceIn(0.75f, 1f))),
                backdropTop = Color.HSVToColor(floatArrayOf(hue, (sat * 0.75f).coerceIn(0.25f, 0.6f), 0.22f)),
                backdropBottom = Color.HSVToColor(floatArrayOf(hue, (sat * 0.6f).coerceIn(0.2f, 0.5f), 0.07f)),
            )
        }
    }
}
