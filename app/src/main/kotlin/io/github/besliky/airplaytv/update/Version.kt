package io.github.besliky.airplaytv.update

/**
 * A version name such as `0.2.0` or `0.3.0-rc.2` (a leading `v` is ignored), ordered the way
 * semantic versioning orders them: by the numbers, then a release above its own test versions,
 * then the test versions by their dot-separated parts.
 */
class Version private constructor(val name: String, private val numbers: List<Int>, private val label: List<String>) :
    Comparable<Version> {

    /** A test version, made before the release of the same number. */
    val isPrerelease: Boolean get() = label.isNotEmpty()

    override fun compareTo(other: Version): Int {
        for (i in 0 until 3) {
            if (numbers[i] != other.numbers[i]) return numbers[i].compareTo(other.numbers[i])
        }
        if (label.isEmpty() != other.label.isEmpty()) return if (label.isEmpty()) 1 else -1
        for (i in 0 until minOf(label.size, other.label.size)) {
            val c = comparePart(label[i], other.label[i])
            if (c != 0) return c
        }
        return label.size.compareTo(other.label.size)
    }

    override fun equals(other: Any?): Boolean = other is Version && compareTo(other) == 0

    override fun hashCode(): Int = numbers.hashCode() * 31 + label.hashCode()

    override fun toString(): String = name

    companion object {
        private val FORMAT = Regex("""^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$""")

        fun parse(text: String): Version? {
            val match = FORMAT.matchEntire(text.trim()) ?: return null
            val (major, minor, patch, label) = match.destructured
            val numbers = listOf(major, minor, patch).map { it.toIntOrNull() ?: return null }
            val parts = if (label.isEmpty()) emptyList() else label.split('.')
            if (parts.any { it.isEmpty() }) return null
            return Version(text.trim().removePrefix("v").substringBefore('+'), numbers, parts)
        }

        /** Numeric parts compare as numbers and sort before words. */
        private fun comparePart(a: String, b: String): Int {
            val x = a.toLongOrNull()
            val y = b.toLongOrNull()
            return when {
                x != null && y != null -> x.compareTo(y)
                x != null -> -1
                y != null -> 1
                else -> a.compareTo(b)
            }
        }
    }
}
