package io.github.jqssun.airplay.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.github.jqssun.airplay.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Idle-screen QR code for the web-push page.
 *
 * The token is deliberately not printed: it is 12 random characters and only makes the caption
 * unreadable, while the QR code itself carries it. Encoding runs off the main thread because the
 * TV builds this bitmap the moment the overview appears.
 *
 * Colours come from the colour scheme so the code belongs to the screen: the card is a scheme
 * surface and the modules are its matching "on" colour. The polarity stays dark-on-light in both
 * themes though — an inverted code (light modules on a dark card) is what the dark palette would
 * naturally give, but a plain zxing reader does not decode one without being told to try the
 * inverted source, and this QR is the only way into the push feature. On the dark theme the light
 * card therefore comes from inverseSurface, which is a palette colour rather than raw white.
 */
@Composable
internal fun WebPushCard(url: String, modifier: Modifier = Modifier) {
    val scheme = MaterialTheme.colorScheme
    val darkBackground = scheme.surface.luminance() < 0.5f
    val background = if (darkBackground) scheme.inverseSurface else scheme.surface
    val content = if (darkBackground) scheme.inverseOnSurface else scheme.onSurface
    val qr by produceState<ImageBitmap?>(null, url, content, background) {
        value = withContext(Dispatchers.Default) {
            QrCode.encode(url, QR_SIZE_PX, dark = content.toArgb(), light = background.toArgb())
                ?.asImageBitmap()
        }
    }
    val caption = url.substringBefore("/?k=").removePrefix("http://").removePrefix("https://")

    Column(modifier = modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            modifier = Modifier
                .clip(RoundedCornerShape(12.dp))
                .background(background)
                .padding(QR_PADDING_DP)
        ) {
            qr?.let {
                Image(
                    bitmap = it,
                    contentDescription = stringResource(R.string.web_push_qr_cd),
                    modifier = Modifier.size(QR_DISPLAY_DP),
                )
            }
        }
        Spacer(Modifier.height(10.dp))
        Text(
            text = stringResource(R.string.web_push_title),
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.85f),
        )
        Spacer(Modifier.height(2.dp))
        Text(
            text = caption,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
            textAlign = TextAlign.Center,
        )
    }
}

private const val QR_SIZE_PX = 512
private val QR_DISPLAY_DP = 168.dp
private val QR_PADDING_DP = 10.dp
