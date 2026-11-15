package io.github.jqssun.airplay.ui

import android.view.Surface
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import io.github.jqssun.airplay.service.MirrorPane
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** ids from here up belong to synthetic panes (see Prefs.DEBUG_PANES), never to a session. */
internal const val DEBUG_PANE_ID_BASE = 9000L

internal data class PaneSlot(val left: Int, val top: Int, val width: Int, val height: Int)

/**
 * Two AirPlay mirror sessions on one screen.
 *
 *  - both phones, same orientation (or any iPad/Mac involved): split 50/50 - each pane is
 *    fitted to its own aspect and the two fitted rectangles are then pulled together to a
 *    small gap and centred as a group, so a pair of portrait phones sits close together
 *    instead of floating apart in the middle of each half
 *  - both phones, one portrait and one landscape: left/right is fixed by the session
 *    order (first one on the left), each pane keeps its aspect and hugs the outer edge
 *    of its own side - rotating a phone must not swap the two pictures
 *
 * Rendering stays full-screen per pane: the GL pipeline blits into whatever Surface it
 * owns, so pane geometry is pure view placement - no business in the renderer.
 */
@Composable
fun MirrorPaneLayout(
    panes: List<MirrorPane>,
    fallback: @Composable () -> Unit,
    onSurfaceAvailable: (Long, Surface) -> Unit,
    onSurfaceDestroyed: (Long, Surface) -> Unit,
    modifier: Modifier = Modifier,
) {
    if (panes.isEmpty()) {
        // no mirror session: the idle preview still uses the primary renderer
        fallback()
        return
    }
    val density = LocalDensity.current
    val minEdgeGap = with(density) { 16.dp.toPx() }
    val minPaneGap = with(density) { 12.dp.toPx() }
    val mixed = _isMixedPhonePair(panes)

    Layout(
        content = {
            panes.forEachIndexed { index, pane ->
                // keyed by session: when one of two senders drops, the surviving pane must
                // keep its own SurfaceView instead of inheriting the other one
                androidx.compose.runtime.key(pane.id) {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        // the portrait pane hugs the outer edge of its own side (see
                        // computePaneSlots); only matters when its width was clamped
                        contentAlignment = when {
                            !mixed || !pane.portrait -> Alignment.Center
                            index == 0 -> Alignment.CenterStart
                            else -> Alignment.CenterEnd
                        }
                    ) {
                        if (pane.id >= DEBUG_PANE_ID_BASE) {
                            // synthetic pane: a tinted stand-in with the same aspect fit as the
                            // real surface, so the geometry is actually visible on screen
                            _DebugPane(pane, index)
                        } else {
                            VideoSurfaceView(
                                onSurfaceAvailable = { onSurfaceAvailable(pane.id, it) },
                                onSurfaceDestroyed = { onSurfaceDestroyed(pane.id, it) },
                                aspectRatio = pane.aspect
                            )
                        }
                    }
                }
            }
        },
        modifier = modifier.fillMaxSize()
    ) { measurables, constraints ->
        val w = constraints.maxWidth
        val h = constraints.maxHeight
        val slots = computePaneSlots(panes, w, h, minEdgeGap, minPaneGap)
        val placeables = measurables.mapIndexed { i, measurable ->
            val slot = slots[i]
            measurable.measure(
                Constraints.fixed(slot.width.coerceAtLeast(1), slot.height.coerceAtLeast(1))
            )
        }
        layout(w, h) {
            placeables.forEachIndexed { i, placeable ->
                placeable.place(slots[i].left, slots[i].top)
            }
        }
    }
}

private fun _isMixedPhonePair(panes: List<MirrorPane>): Boolean =
    panes.size >= 2 && panes.all { it.isPhone } && panes[0].portrait != panes[1].portrait

internal fun computePaneSlots(
    panes: List<MirrorPane>,
    width: Int,
    height: Int,
    minEdgeGap: Float,
    minPaneGap: Float,
): List<PaneSlot> {
    // a little breathing room from the bezel and between the two panes
    val edge = max(minEdgeGap, width * 0.02f).roundToInt()
    val gap = max(minPaneGap, width * 0.015f).roundToInt()
    val contentW = (width - 2 * edge).coerceAtLeast(1)
    val contentH = (height - 2 * edge).coerceAtLeast(1)

    if (_isMixedPhonePair(panes)) {
        val portraitIndex = if (panes[0].portrait) 0 else 1
        val portraitAspect = panes[portraitIndex].aspect
        // tall side keeps the phone's aspect; clamp so the landscape pane never vanishes
        val portraitW = min(
            (contentH * portraitAspect).roundToInt(),
            (contentW * 0.45f).roundToInt()
        ).coerceAtLeast(1)
        val landscapeW = (contentW - portraitW - gap).coerceAtLeast(1)
        // Left/right belongs to the session, not to the orientation: pane 0 stays on the
        // left and pane 1 on the right even when one of them rotates. Pinning "portrait
        // always left" made the two devices swap sides the moment a phone was turned.
        // Each pane keeps its aspect and hugs the outer edge of its own side.
        return List(panes.size) { i ->
            val w = if (i == portraitIndex) portraitW else landscapeW
            val left = if (i == 0) edge else width - edge - w
            PaneSlot(left, edge, w, contentH)
        }
    }

    if (panes.size == 1) {
        // single session: whole screen, aspect fitted and centred - legacy look
        return listOf(PaneSlot(0, 0, width, height))
    }

    // Panes narrower than their half (portrait phones on a landscape TV are height-bound)
    // get pulled together to this gap instead of being centred inside their half - centring
    // them used to leave ~220px of dead space per side, i.e. a 25%-of-screen hole in the
    // middle that reads as "the two phones are miles apart".
    val packGap = max(minPaneGap, width * 0.05f).roundToInt()
    val halfW = ((contentW - packGap) / 2).coerceAtLeast(1)

    // fit every pane into its half first ...
    val fitted = panes.map { pane ->
        val aspect = if (pane.aspect > 0.01f) pane.aspect else 1f
        val fw = min(halfW, (contentH * aspect).roundToInt()).coerceAtLeast(1)
        val fh = min(contentH, (fw / aspect).roundToInt()).coerceAtLeast(1)
        fw to fh
    }
    // ... then place the fitted rectangles side by side with exactly packGap between them
    // and centre the whole group, so the two pictures read as a pair. When both panes fill
    // their half (landscape) this reproduces the plain 50/50 split.
    val totalW = fitted.sumOf { it.first } + packGap * (fitted.size - 1)
    val slots = ArrayList<PaneSlot>(fitted.size)
    var x = ((width - totalW) / 2f).roundToInt().coerceAtLeast(0)
    for ((fw, fh) in fitted) {
        slots.add(PaneSlot(x, edge + ((contentH - fh) / 2f).roundToInt(), fw, fh))
        x += fw + packGap
    }
    return slots
}

@Composable
private fun _DebugPane(pane: MirrorPane, index: Int) {
    val fill = if (index % 2 == 0) Color(0xFF1D6F5E) else Color(0xFF7A5A18)
    val shape = RoundedCornerShape(6.dp)
    Box(
        modifier = Modifier
            .aspectRatio(if (pane.aspect > 0.01f) pane.aspect else 1f)
            .background(fill, shape)
            .border(1.dp, Color.White.copy(alpha = 0.35f), shape),
        contentAlignment = Alignment.Center
    ) {
        Text(
            "${pane.deviceName}\n${pane.resolution}",
            color = Color.White,
            textAlign = TextAlign.Center,
            style = MaterialTheme.typography.bodySmall
        )
    }
}
