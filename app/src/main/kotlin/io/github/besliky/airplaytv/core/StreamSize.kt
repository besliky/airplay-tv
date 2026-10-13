package io.github.besliky.airplaytv.core

/**
 * The display size advertised to senders when the picture should fill a screen that is not 16:9,
 * as in cars (1024x600, 1280x480, 1920x720, 2000x1200 ...). The sender scales its own screen to fit
 * this size, so advertising the panel's real shape gives the largest picture at the lowest bitrate.
 */
object StreamSize {

    /**
     * [screenWidth] x [screenHeight] in landscape, scaled down so that the long side is at most
     * [maxLong] and rounded to even numbers; 1920x1080 when the screen is unknown.
     */
    fun fit(screenWidth: Int, screenHeight: Int, maxLong: Int): Pair<Int, Int> {
        var long = maxOf(screenWidth, screenHeight)
        var short = minOf(screenWidth, screenHeight)
        if (short < MIN_SHORT || long < MIN_LONG) return 1920 to 1080
        if (long > maxLong) {
            short = (short.toLong() * maxLong / long).toInt()
            long = maxLong
        }
        return (long and 1.inv()) to (maxOf(short, MIN_SHORT) and 1.inv())
    }

    private const val MIN_SHORT = 240
    private const val MIN_LONG = 320
}
