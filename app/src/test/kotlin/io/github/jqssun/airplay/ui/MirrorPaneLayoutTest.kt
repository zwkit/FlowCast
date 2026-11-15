package io.github.jqssun.airplay.ui

import io.github.jqssun.airplay.service.MirrorPane
import kotlin.math.roundToInt
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Geometry of the 2.0 dual-pane layout (1920x1080 screen, 16dp/12dp minimum gaps). */
class MirrorPaneLayoutTest {

    private val w = 1920
    private val h = 1080
    private val minEdge = 16f * 2f   // px-ish; the function rescales by screen width anyway
    private val minGap = 12f * 2f

    private fun pane(id: Long, aspect: Float, phone: Boolean = true) =
        MirrorPane(id, aspect, "", if (phone) "iPhone" else "MacBook", phone, false)

    private fun slots(panes: List<MirrorPane>) = computePaneSlots(panes, w, h, minEdge, minGap)

    @Test
    fun `two portrait phones are pulled together and centred as a pair`() {
        val s = slots(listOf(pane(1, 9f / 19.5f), pane(2, 9f / 19.5f)))
        assertEquals(s[0].width, s[1].width)
        // the visible gap is the pack gap (3% of the screen width) - centring each pane
        // inside its own half used to leave ~473px of dead space between them
        assertEquals("pack gap", (w * 0.05f).roundToInt(), s[1].left - (s[0].left + s[0].width))
        assertEquals("pair centred", s[0].left, w - (s[1].left + s[1].width))
        assertEquals(s[0].top, s[1].top)
        assertEquals(s[0].height, s[1].height)
        assertTrue(s[0].left > 0 && s[0].top > 0)
    }

    @Test
    fun `two landscape phones also split in half`() {
        val s = slots(listOf(pane(1, 16f / 9f), pane(2, 16f / 9f)))
        assertEquals(s[0].width, s[1].width)
        assertEquals("pack gap", (w * 0.05f).roundToInt(), s[1].left - (s[0].left + s[0].width))
        assertEquals("pair centred", s[0].left, w - (s[1].left + s[1].width))
    }

    @Test
    fun `mixed pair keeps the first session on the left, portrait hugging its own edge`() {
        // landscape first (left), portrait second (right)
        val a = slots(listOf(pane(1, 16f / 9f), pane(2, 9f / 19.5f)))
        assertTrue("pane 0 stays left", a[0].left < w / 2)
        assertTrue("pane 1 sits right of pane 0", a[1].left >= a[0].left + a[0].width)
        assertEquals("portrait pane hugs the right edge", w - (a[1].left + a[1].width), a[0].left)
        assertAspect(a[1], 9f / 19.5f)
        assertTrue("portrait pane never eats the landscape one", a[0].width > a[1].width)

        // portrait first (left), landscape second (right): the mirror image
        val b = slots(listOf(pane(1, 9f / 19.5f), pane(2, 16f / 9f)))
        assertEquals("portrait pane hugs the left edge", b[0].left, w - (b[1].left + b[1].width))
        assertAspect(b[0], 9f / 19.5f)
        assertTrue(b[0].left < w / 2)
        assertTrue(b[1].left >= b[0].left + b[0].width)
    }

    @Test
    fun `ipad or mac makes both panes share the screen like landscape phones`() {
        val s = slots(listOf(pane(1, 3f / 4f, phone = false), pane(2, 16f / 10f, phone = false)))
        // same 50/50 branch as the phones: each pane keeps its own aspect, packed together
        assertAspect(s[0], 3f / 4f)
        assertAspect(s[1], 16f / 10f)
        assertEquals("pack gap", (w * 0.05f).roundToInt(), s[1].left - (s[0].left + s[0].width))
        // panes of different widths can leave one stray pixel on one side
        assertTrue("pair centred", kotlin.math.abs(s[0].left - (w - (s[1].left + s[1].width))) <= 1)
    }

    private fun assertAspect(slot: PaneSlot, aspect: Float) {
        val actual = slot.width.toFloat() / slot.height.toFloat()
        assertTrue("aspect $actual != $aspect", kotlin.math.abs(actual - aspect) < 0.01f)
    }

    @Test
    fun `a single session keeps the whole screen`() {
        val s = slots(listOf(pane(1, 9f / 19.5f)))
        assertEquals(1, s.size)
        assertEquals(0, s[0].left)
        assertEquals(0, s[0].top)
        assertEquals(w, s[0].width)
        assertEquals(h, s[0].height)
    }
}
