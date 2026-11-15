package io.github.jqssun.airplay.renderer

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The video renderer schedules each decoded frame at the wall-clock instant that
 * matches its NTP pts, using an anchor shared with the audio playout clock. These
 * tests lock the math (and the offset nudge) so a regression can't silently
 * reintroduce per-stream timelines.
 */
class VideoRendererSyncTest {

    @Test
    fun `anchor frame renders at the anchor wall instant`() {
        // anchor: pts 1s (us) played at wall 10s (ns)
        val renderNs = VideoRenderer.scheduledRenderNs(
            anchorPtsUs = 1_000_000L,
            anchorWallNs = 10_000_000_000L,
            framePtsUs = 1_000_000L,
            offsetNs = 0L,
        )
        assertEquals(10_000_000_000L, renderNs)
    }

    @Test
    fun `later frame renders one frame interval later`() {
        val renderNs = VideoRenderer.scheduledRenderNs(
            anchorPtsUs = 1_000_000L,
            anchorWallNs = 10_000_000_000L,
            framePtsUs = 1_000_000L + 16_700L,  // one 60fps frame later
            offsetNs = 0L,
        )
        assertEquals(10_000_000_000L + 16_700_000L, renderNs)
    }

    @Test
    fun `positive offset delays the picture, negative advances it`() {
        val base = VideoRenderer.scheduledRenderNs(0L, 0L, 0L, 0L)
        assertEquals(16_000_000L, VideoRenderer.scheduledRenderNs(0L, 0L, 0L, 16_000_000L) - base)
        assertEquals(-16_000_000L, VideoRenderer.scheduledRenderNs(0L, 0L, 0L, -16_000_000L) - base)
    }

    @Test
    fun `real-sized NTP pts does not overflow`() {
        // ntp_time_local is nanoseconds since epoch; after /1000 it is ~1.7e15 us
        val anchorPtsUs = 1_700_000_000_000_000L
        val framePtsUs = anchorPtsUs + 16_700L
        val renderNs = VideoRenderer.scheduledRenderNs(
            anchorPtsUs = anchorPtsUs,
            anchorWallNs = 123_456_789L,
            framePtsUs = framePtsUs,
            offsetNs = 0L,
        )
        assertEquals(123_456_789L + 16_700_000L, renderNs)
    }

    @Test
    fun `epoch sentinel never collides with a native epoch`() {
        // native epochs start at 0 and only grow; self-anchoring must be replaced
        // as soon as an audio anchor shows up
        assertEquals(Int.MAX_VALUE, VideoRenderer.EPOCH_SELF)
    }

    @Test
    fun `normal decode-ahead is not mistaken for a bogus anchor`() {
        val now = 50_000_000_000L
        assertEquals(false, VideoRenderer.leadsTooFar(now, now))            // due now
        assertEquals(false, VideoRenderer.leadsTooFar(now + 300_000_000L, now))  // 300ms early
        assertEquals(false, VideoRenderer.leadsTooFar(now - 200_000_000L, now))  // late: shows at once
    }

    @Test
    fun `anchor from another clock domain is rejected`() {
        // A TS-carried stream keeps pts in its own clock (~10^12 ns) while a leftover AirPlay
        // anchor sits near 10^18 ns: honouring it would park frames far in the future
        val now = 50_000_000_000L
        assertEquals(true, VideoRenderer.leadsTooFar(now + 60_000_000_000L, now))   // +60s
        assertEquals(true, VideoRenderer.leadsTooFar(Long.MAX_VALUE / 4, now))
    }
}
