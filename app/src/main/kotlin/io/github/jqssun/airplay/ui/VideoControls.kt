package io.github.jqssun.airplay.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.AspectRatio
import androidx.compose.material.icons.rounded.CropLandscape
import androidx.compose.material.icons.rounded.FastForward
import androidx.compose.material.icons.rounded.FitScreen
import androidx.compose.material.icons.rounded.Pause
import androidx.compose.material.icons.rounded.PhotoSizeSelectActual
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Speed
import androidx.compose.material.ripple.RippleAlpha
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.IconButton
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import io.github.jqssun.airplay.R
import io.github.jqssun.airplay.renderer.VideoFormatInfo
import io.github.jqssun.airplay.ui.gestures.VideoContentScale
import io.github.jqssun.airplay.viewmodel.MainViewModel
import kotlin.math.abs
import kotlin.math.roundToInt

// white content, strong white ripple
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PlayerButton(
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
    containerColor: Color = Color.Transparent,
    content: @Composable () -> Unit,
) {
    // the click must go through IconButton's own clickable: a remote's OK key activates a focused
    // clickable directly and never emits a PressInteraction, so the old interaction-listening
    // version silently did nothing on a TV remote
    CompositionLocalProvider(
        LocalContentColor provides Color.White,
        LocalRippleConfiguration provides RippleConfiguration(
            color = Color.White,
            rippleAlpha = RippleAlpha(
                draggedAlpha = 0.5f,
                focusedAlpha = 0.5f,
                hoveredAlpha = 0.5f,
                pressedAlpha = 0.5f
            )
        )
    ) {
        IconButton(
            onClick = onClick,
            modifier = modifier.focusRing(),
            colors = IconButtonDefaults.iconButtonColors().copy(containerColor = containerColor),
            content = content,
        )
    }
}

@Composable
fun PlayPauseButton(
    playing: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 64.dp,
) {
    PlayerButton(modifier = modifier.size(size), onClick = onClick) {
        Icon(
            imageVector = if (playing) Icons.Rounded.Pause else Icons.Rounded.PlayArrow,
            contentDescription = stringResource(R.string.cd_play_pause),
            modifier = Modifier.size(size * 0.75f)
        )
    }
}

// TV-first top bar: a back affordance and the title. Speed / copy-url / download were removed —
// on a TV none of them earned their place on screen (a remote cannot use them, and the receiver
// is not the place to download a sender's file).
@Composable
fun VideoControlsTop(
    title: String,
    onBackClick: () -> Unit,
    modifier: Modifier = Modifier,
    backModifier: Modifier = Modifier,
) {
    val insets = WindowInsets.systemBars.union(WindowInsets.displayCutout).asPaddingValues()
    val layoutDirection = LocalLayoutDirection.current
    val extraTopPadding = if (insets.calculateTopPadding() == 0.dp) 16.dp else 0.dp
    Row(
        modifier = modifier
            .padding(
                start = insets.calculateStartPadding(layoutDirection),
                top = insets.calculateTopPadding(),
                end = insets.calculateEndPadding(layoutDirection),
            )
            .padding(horizontal = 8.dp)
            .padding(bottom = 16.dp)
            .padding(top = extraTopPadding),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        PlayerButton(onClick = onBackClick, modifier = backModifier) {
            Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = stringResource(R.string.cd_back))
        }
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            color = Color.White,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
    }
}

/**
 * Bottom bar: transport + progress + stream info, nothing else.
 *
 * Layout mirrors what a TV viewer expects from a set-top player — play/pause sits at the left end
 * of the progress bar, the elapsed/total time follows it, and the codec line (resolution, codec,
 * frame rate) is parked at the right end of the same row.
 */
@Composable
fun VideoControlsBottom(
    playing: Boolean,
    positionMs: Long,
    durationMs: Long,
    videoInfo: String,
    onPlayPauseClick: () -> Unit,
    onSeek: (Long) -> Unit,
    onSeekEnd: () -> Unit,
    modifier: Modifier = Modifier,
    seekBarModifier: Modifier = Modifier,
    playPauseModifier: Modifier = Modifier,
) {
    val insets = WindowInsets.systemBars.union(WindowInsets.displayCutout).asPaddingValues()
    val layoutDirection = LocalLayoutDirection.current
    Column(
        modifier = modifier
            .padding(
                start = insets.calculateStartPadding(layoutDirection),
                end = insets.calculateEndPadding(layoutDirection),
                bottom = insets.calculateBottomPadding(),
            )
            .padding(horizontal = 8.dp)
            .padding(top = 16.dp)
            .padding(bottom = 16.dp.takeIf { insets.calculateBottomPadding() == 0.dp } ?: 0.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Row(
            modifier = Modifier.padding(start = 4.dp, end = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PlayPauseButton(
                playing = playing,
                onClick = onPlayPauseClick,
                modifier = playPauseModifier,
                size = 44.dp,
            )
            Spacer(Modifier.width(4.dp))
            var showRemaining by rememberSaveable { mutableStateOf(false) }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                ) { showRemaining = !showRemaining },
            ) {
                Text(
                    text = if (showRemaining) "-${formatVideoTime(durationMs - positionMs)}" else formatVideoTime(positionMs),
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color.White,
                )
                Text(text = " / ", style = MaterialTheme.typography.bodyMedium, color = Color.White)
                Text(
                    text = formatVideoTime(durationMs),
                    style = MaterialTheme.typography.bodyMedium,
                    color = Color.White.copy(alpha = 0.65f),
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            if (videoInfo.isNotEmpty()) {
                Text(
                    text = videoInfo,
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.White.copy(alpha = 0.75f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        PlayerSeekbar(
            positionMs = positionMs,
            durationMs = durationMs,
            onSeek = onSeek,
            onSeekEnd = onSeekEnd,
            modifier = seekBarModifier,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PlayerSeekbar(
    positionMs: Long,
    durationMs: Long,
    onSeek: (Long) -> Unit,
    onSeekEnd: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var isFocused by remember { mutableStateOf(false) }
    val thumbSize by animateDpAsState(if (isFocused) 22.dp else 16.dp, label = "thumbSize")
    CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
        Slider(
            value = positionMs.toFloat().coerceIn(0f, durationMs.toFloat()),
            valueRange = 0f..durationMs.toFloat(),
            onValueChange = { onSeek(it.toLong()) },
            onValueChangeFinished = onSeekEnd,
            modifier = modifier
                .fillMaxWidth()
                .onFocusChanged { isFocused = it.isFocused }
                .height(24.dp),
            thumb = {
                Box(
                    modifier = Modifier
                        .size(thumbSize)
                        .shadow(4.dp, CircleShape)
                        .background(Color.White)
                        .then(
                            if (isFocused) {
                                Modifier.border(2.dp, MaterialTheme.colorScheme.primary, CircleShape)
                            } else Modifier
                        ),
                )
            },
            track = {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(4.dp)
                        .clip(MaterialTheme.shapes.extraSmall)
                        .background(Color.White.copy(alpha = 0.5f))
                ) {
                    if (durationMs > 0) {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth((positionMs.toFloat() / durationMs).coerceIn(0f, 1f))
                                .height(4.dp)
                                .background(MaterialTheme.colorScheme.primary)
                        )
                    }
                }
            }
        )
    }
}

// sizes the surface per content scale; requiredSize lets crop/100% exceed the container
@Composable
fun VideoContentFrame(
    aspect: Float,
    videoSizePx: Pair<Int, Int>?,
    contentScale: VideoContentScale,
    zoom: Float,
    modifier: Modifier = Modifier,
    content: @Composable (Modifier) -> Unit,
) {
    BoxWithConstraints(modifier = modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        val w = maxWidth
        val h = maxHeight
        val fit = if (w / h > aspect) Modifier.requiredSize(h * aspect, h) else Modifier.requiredSize(w, w / aspect)
        val sizeModifier = when (contentScale) {
            VideoContentScale.BEST_FIT -> fit
            VideoContentScale.STRETCH -> Modifier.requiredSize(w, h)
            VideoContentScale.CROP ->
                if (w / h > aspect) Modifier.requiredSize(w, w / aspect) else Modifier.requiredSize(h * aspect, h)
            VideoContentScale.HUNDRED_PERCENT -> videoSizePx?.let { (pxW, pxH) ->
                with(LocalDensity.current) { Modifier.requiredSize(pxW.toDp(), pxH.toDp()) }
            } ?: fit
        }
        content(
            sizeModifier.graphicsLayer {
                scaleX = zoom
                scaleY = zoom
            }
        )
    }
}

// key decision table for a TV remote:
//  - OK (center/enter) toggles playback and brings the bar up — it never depends on where the
//    focus happens to be, which is what made "OK does nothing" so easy to hit before
//  - arrows navigate the bar when it is up, and seek directly when it is not
fun handlePlayerKeyEvent(
    keyEvent: KeyEvent,
    controlsVisible: Boolean,
    isBackFocused: Boolean,
    seekIncrementMs: Long,
    viewModel: MainViewModel,
    showControls: () -> Unit,
    onDpadSeek: (deltaMs: Long) -> Unit,
): Boolean {
    if (keyEvent.type != KeyEventType.KeyDown) return false

    return when (keyEvent.key) {
        Key.MediaPlayPause, Key.Spacebar -> { viewModel.toggleVideoPlayPause(); showControls(); true }
        Key.MediaPlay -> { viewModel.setVideoPlaying(true); showControls(); true }
        Key.MediaPause -> { viewModel.setVideoPlaying(false); showControls(); true }
        Key.MediaFastForward -> { viewModel.seekVideoBy(seekIncrementMs); showControls(); true }
        Key.MediaRewind -> { viewModel.seekVideoBy(-seekIncrementMs); showControls(); true }
        Key.MediaStop -> { viewModel.stopVideoPlayback(); true }
        Key.DirectionCenter, Key.Enter, Key.NumPadEnter -> {
            // the back arrow is focused: let its own click handler run (it leaves the video)
            if (controlsVisible && isBackFocused) {
                false
            } else {
                viewModel.toggleVideoPlayPause()
                showControls()
                true
            }
        }
        Key.DirectionLeft -> {
            if (!controlsVisible) {
                viewModel.seekVideoBy(-seekIncrementMs)
                onDpadSeek(-seekIncrementMs)
                true
            } else {
                showControls()
                false
            }
        }
        Key.DirectionRight -> {
            if (!controlsVisible) {
                viewModel.seekVideoBy(seekIncrementMs)
                onDpadSeek(seekIncrementMs)
                true
            } else {
                showControls()
                false
            }
        }
        Key.DirectionUp, Key.DirectionDown -> {
            showControls()
            !controlsVisible
        }
        else -> false
    }
}

// cumulative amount skipped by repeated dpad seeks while controls are hidden
@Composable
fun BoxScope.DpadSeekIndicator(
    visible: Boolean,
    offsetMs: Long,
    positionMs: Long,
) {
    AnimatedVisibility(
        modifier = Modifier.align(Alignment.Center),
        visible = visible,
        enter = fadeIn(),
        exit = fadeOut(),
    ) {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = Color.Black.copy(alpha = 0.6f),
        ) {
            Row(
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Icon(
                    imageVector = Icons.Rounded.FastForward,
                    contentDescription = null,
                    tint = Color.White,
                    modifier = Modifier
                        .size(28.dp)
                        .rotate(if (offsetMs < 0) 180f else 0f),
                )
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = (if (offsetMs >= 0) "+" else "-") +
                            stringResource(R.string.seconds_short, abs(offsetMs) / 1000),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                        color = Color.White,
                    )
                    Text(
                        text = formatVideoTime(positionMs),
                        style = MaterialTheme.typography.bodyMedium,
                        color = Color.White.copy(alpha = 0.8f),
                    )
                }
            }
        }
    }
}

fun VideoContentScale.nameRes(): Int = when (this) {
    VideoContentScale.BEST_FIT -> R.string.scale_best_fit
    VideoContentScale.STRETCH -> R.string.scale_stretch
    VideoContentScale.CROP -> R.string.scale_crop
    VideoContentScale.HUNDRED_PERCENT -> R.string.scale_hundred_percent
}

fun VideoContentScale.icon(): ImageVector = when (this) {
    VideoContentScale.BEST_FIT -> Icons.Rounded.FitScreen
    VideoContentScale.STRETCH -> Icons.Rounded.AspectRatio
    VideoContentScale.CROP -> Icons.Rounded.CropLandscape
    VideoContentScale.HUNDRED_PERCENT -> Icons.Rounded.PhotoSizeSelectActual
}

/**
 * One-line stream summary for the player bar, e.g. "1920×1080 · H.264 · 30 fps".
 * Parts that are not known yet are skipped, so a slow probe never leaves stray separators.
 */
fun videoInfoLabel(format: VideoFormatInfo): String {
    val parts = buildList {
        if (format.width > 0 && format.height > 0) add("${format.width}×${format.height}")
        codecLabel(format.codecs)?.let { add(it) }
        val fps = format.fps.roundToInt()
        if (fps in 1..240) add("$fps fps")
    }
    return parts.joinToString(" · ")
}

// track codecs are RFC 6381 strings ("avc1.640028", "hev1.1.6.L93.B0"); show the family instead
private fun codecLabel(codecs: String): String? {
    if (codecs.isBlank()) return null
    val c = codecs.lowercase()
    return when {
        c.startsWith("avc") || c.contains("h264") -> "H.264"
        c.startsWith("hev") || c.startsWith("hvc") || c.contains("hevc") || c.contains("h265") -> "H.265"
        c.startsWith("vp09") || c.contains("vp9") -> "VP9"
        c.startsWith("av01") || c.contains("av1") -> "AV1"
        c.startsWith("mp4v") -> "MPEG-4"
        c.contains("mpeg2") -> "MPEG-2"
        else -> codecs.substringBefore('.').uppercase()
    }
}

internal fun formatVideoTime(ms: Long): String {
    val s = (ms / 1000).toInt().coerceAtLeast(0)
    return if (s >= 3600) "%d:%02d:%02d".format(s / 3600, (s % 3600) / 60, s % 60)
    else "%d:%02d".format(s / 60, s % 60)
}
