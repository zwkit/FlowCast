package io.github.jqssun.airplay.ui

import android.graphics.Bitmap
import android.graphics.Color
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter

/**
 * QR rendering for the web-push entry point. zxing is vendored as a local jar (see
 * app/libs), and the encoder is only exercised when the idle screen is visible.
 *
 * The module colours are passed in rather than hard-coded: the card follows the app palette (see
 * WebPushCard), but the polarity stays dark-on-light — an inverted code is only readable by
 * scanners that bother to retry with an inverted source, and this QR is the sole entry point to
 * the push feature. Callers must hand over a pair with scheme-level contrast, not decorative
 * tints, or the TV's own colour processing eats the code.
 */
internal object QrCode {
    fun encode(
        content: String,
        sizePx: Int,
        dark: Int = Color.BLACK,
        light: Int = Color.WHITE,
    ): Bitmap? {
        if (content.isEmpty() || sizePx <= 0) return null
        return runCatching {
            val hints = mapOf(
                EncodeHintType.MARGIN to 1,
                EncodeHintType.CHARACTER_SET to "UTF-8",
            )
            val matrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, sizePx, sizePx, hints)
            val w = matrix.width
            val h = matrix.height
            val pixels = IntArray(w * h)
            for (y in 0 until h) {
                val row = y * w
                for (x in 0 until w) pixels[row + x] = if (matrix[x, y]) dark else light
            }
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                .apply { setPixels(pixels, 0, w, 0, 0, w, h) }
        }.getOrNull()
    }
}
