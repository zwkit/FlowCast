package io.github.jqssun.airplay.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import kotlinx.coroutines.delay
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import io.github.jqssun.airplay.Locales
import io.github.jqssun.airplay.Prefs
import io.github.jqssun.airplay.R
import io.github.jqssun.airplay.viewmodel.MainViewModel
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import kotlin.math.roundToInt

/**
 * Settings as a full-screen page: it replaces the home page instead of floating over it,
 * so long rows get the whole width and nothing competes for attention behind it.
 */
@Composable
fun SettingsScreen(
    viewModel: MainViewModel,
    onOpenLogs: (() -> Unit)? = null,
    onBack: () -> Unit = {},
    // stops the receiver service and closes the app; back no longer does this
    onQuit: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        shape = RectangleShape,
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 0.dp,
        shadowElevation = 0.dp,
    ) {
        SettingsScreenContent(viewModel, onOpenLogs, onBack, onQuit)
    }
}

@Composable
private fun SettingsScreenContent(
    viewModel: MainViewModel,
    onOpenLogs: (() -> Unit)? = null,
    onBack: () -> Unit = {},
    onQuit: (() -> Unit)? = null,
) {
    val appLanguage by viewModel.pref(Prefs.APP_LANGUAGE).collectAsState()
    val serverName by viewModel.pref(Prefs.SERVER_NAME).collectAsState()
    val h265Enabled by viewModel.pref(Prefs.H265_ENABLED).collectAsState()
    val enforceSdr by viewModel.pref(Prefs.ENFORCE_SDR).collectAsState()
    val alacEnabled by viewModel.pref(Prefs.ALAC_ENABLED).collectAsState()
    val aacEnabled by viewModel.pref(Prefs.AAC_ENABLED).collectAsState()
    val resolution by viewModel.pref(Prefs.RESOLUTION).collectAsState()
    val idlePreview by viewModel.pref(Prefs.IDLE_PREVIEW).collectAsState()
    val autoFullscreen by viewModel.pref(Prefs.AUTO_FULLSCREEN).collectAsState()
    val keepScreenOn by viewModel.pref(Prefs.KEEP_SCREEN_ON).collectAsState()
    val advertiseVideo by viewModel.pref(Prefs.ADVERTISE_VIDEO).collectAsState()
    val advertiseMirroring by viewModel.pref(Prefs.ADVERTISE_MIRRORING).collectAsState()
    val advertiseAudio by viewModel.pref(Prefs.ADVERTISE_AUDIO).collectAsState()
    val launchOnConnect by viewModel.pref(Prefs.LAUNCH_ON_CONNECT).collectAsState()
    val maxFps by viewModel.pref(Prefs.MAX_FPS).collectAsState()
    val overscanned by viewModel.pref(Prefs.OVERSCANNED).collectAsState()
    val requirePin by viewModel.pref(Prefs.REQUIRE_PIN).collectAsState()
    val allowNewConn by viewModel.pref(Prefs.ALLOW_NEW_CONN).collectAsState()
    val multiSession by viewModel.pref(Prefs.MULTI_SESSION).collectAsState()
    val webPush by viewModel.pref(Prefs.WEB_PUSH).collectAsState()
    val debugPanes by viewModel.pref(Prefs.DEBUG_PANES).collectAsState()
    val usbOnly by viewModel.pref(Prefs.USB_ONLY).collectAsState()
    val exitOnCastEnd by viewModel.pref(Prefs.EXIT_ON_CAST_END).collectAsState()
    val theme by viewModel.pref(Prefs.THEME).collectAsState()
    val autoStart by viewModel.pref(Prefs.AUTO_START).collectAsState()
    val bootAutoStart by viewModel.pref(Prefs.BOOT_AUTO_START).collectAsState()
    val runInBackground by viewModel.pref(Prefs.RUN_IN_BACKGROUND).collectAsState()
    val serverPort by viewModel.pref(Prefs.SERVER_PORT).collectAsState()
    val audioLatencyMs by viewModel.pref(Prefs.AUDIO_LATENCY_MS).collectAsState()
    val forceSwAlac by viewModel.pref(Prefs.FORCE_SW_ALAC).collectAsState()
    val forceSwAac by viewModel.pref(Prefs.FORCE_SW_AAC).collectAsState()
    val debugEnabled by viewModel.pref(Prefs.DEBUG_ENABLED).collectAsState()
    val developerOptions by viewModel.pref(Prefs.DEVELOPER_OPTIONS).collectAsState()
    val keyAllowFrameDrop by viewModel.pref(Prefs.KEY_ALLOW_FRAME_DROP).collectAsState()
    val realtimeDecoderPriority by viewModel.pref(Prefs.KEY_PRIORITY).collectAsState()
    val lowLatency by viewModel.pref(Prefs.LOW_LATENCY).collectAsState()
    val operatingRate by viewModel.pref(Prefs.OPERATING_RATE).collectAsState()
    val scheduledOutputBufferRelease by viewModel.pref(Prefs.SCHEDULED_OUTPUT_BUFFER_RELEASE).collectAsState()
    val avSyncOffsetMs by viewModel.pref(Prefs.AV_SYNC_OFFSET_MS).collectAsState()
    val benchmarkLog by viewModel.pref(Prefs.BENCHMARK_LOG).collectAsState()
    val audioAutoBuffer by viewModel.pref(Prefs.AUDIO_AUTO_BUFFER).collectAsState()
    val audioCushionMs by viewModel.pref(Prefs.AUDIO_CUSHION_MS).collectAsState()
    val audioAdaptiveStep by viewModel.pref(Prefs.AUDIO_ADAPTIVE_STEP).collectAsState()
    val oboeBufferFrames by viewModel.pref(Prefs.OBOE_BUFFER_FRAMES).collectAsState()

    val backFocus = remember { FocusRequester() }
    val tv = isTv()
    LaunchedEffect(Unit) {
        if (tv) backFocus.requestFocus()
    }
    Column(modifier = Modifier.fillMaxSize()) {
        // the bottom tab bar is gone: the settings page gets its own header with a visible
        // back affordance (the system back key still works and is handled by MainScreen)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 4.dp, end = 16.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(
                onClick = onBack,
                modifier = Modifier.dpadFocus().focusRequester(backFocus)
            ) {
                Icon(
                    imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = stringResource(R.string.btn_back)
                )
            }
            Text(
                text = stringResource(R.string.tab_settings),
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.SemiBold
            )
        }
        HorizontalDivider()
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(vertical = 8.dp)
        ) {
        val activity = LocalContext.current as? android.app.Activity

        SettingSegmented(
            title = stringResource(R.string.setting_language),
            summary = stringResource(R.string.setting_language_desc),
            value = appLanguage,
            options = listOf(
                Locales.ZH to stringResource(R.string.language_zh),
                Locales.EN to stringResource(R.string.language_en),
                Locales.SYSTEM to stringResource(R.string.language_system),
            ),
            onValueChange = { v ->
                viewModel.set(Prefs.APP_LANGUAGE, v)
                activity?.recreate()
            }
        )

        SectionHeader(stringResource(R.string.section_server))

        SettingTextField(
            label = stringResource(R.string.setting_server_name),
            // empty stored value means "use the device's own name"; surface that default here
            value = serverName.ifBlank { Prefs.defaultDeviceName },
            onCommit = { viewModel.set(Prefs.SERVER_NAME, it) }
        )

        SettingTextField(
            label = stringResource(R.string.setting_server_port),
            value = serverPort.toString(),
            onCommit = { viewModel.set(Prefs.SERVER_PORT, it.toInt()) },
            range = Prefs.SERVER_PORT.range
        )

        SettingSwitch(
            title = stringResource(R.string.setting_boot_auto_start),
            description = stringResource(R.string.setting_boot_auto_start_desc),
            checked = bootAutoStart,
            onCheckedChange = { viewModel.set(Prefs.BOOT_AUTO_START, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_run_in_background),
            description = stringResource(R.string.setting_run_in_background_desc),
            checked = runInBackground,
            onCheckedChange = { viewModel.set(Prefs.RUN_IN_BACKGROUND, it) }
        )

        SectionHeader(stringResource(R.string.section_connection))

        SettingSegmented(
            title = stringResource(R.string.setting_discovery),
            summary = stringResource(if (usbOnly) R.string.setting_discovery_usb_desc else R.string.setting_discovery_all_desc),
            value = usbOnly,
            options = listOf(
                false to stringResource(R.string.chip_all_networks),
                true to stringResource(R.string.chip_usb_only),
            ),
            onValueChange = { viewModel.set(Prefs.USB_ONLY, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_require_pin),
            description = stringResource(R.string.setting_require_pin_desc),
            checked = requirePin,
            onCheckedChange = { viewModel.set(Prefs.REQUIRE_PIN, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_allow_new_conn),
            description = stringResource(R.string.setting_allow_new_conn_desc),
            checked = allowNewConn,
            onCheckedChange = { viewModel.set(Prefs.ALLOW_NEW_CONN, it) }
        )

        // 2.0: two mirror senders side by side. Off restores the classic exclusive receiver
        // (a second device gets the "server is busy" reply instead of taking half the screen).
        SettingSwitch(
            title = stringResource(R.string.setting_multi_session),
            description = stringResource(R.string.setting_multi_session_desc),
            checked = multiSession,
            onCheckedChange = { viewModel.set(Prefs.MULTI_SESSION, it) }
        )

        // 2.1: the idle screen shows a QR code that opens a tiny LAN page on the phone
        SettingSwitch(
            title = stringResource(R.string.setting_web_push),
            description = stringResource(R.string.setting_web_push_desc),
            checked = webPush,
            onCheckedChange = { viewModel.set(Prefs.WEB_PUSH, it) }
        )

        val ctx = LocalContext.current
        val lifecycleOwner = LocalLifecycleOwner.current
        var hasOverlayPermission by remember { mutableStateOf(canAutoLaunch(ctx)) }
        DisposableEffect(lifecycleOwner) {
            val observer = LifecycleEventObserver { _, event ->
                if (event == Lifecycle.Event.ON_RESUME) hasOverlayPermission = canAutoLaunch(ctx)
            }
            lifecycleOwner.lifecycle.addObserver(observer)
            onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
        }
        val needsOverlayPermission = launchOnConnect && !hasOverlayPermission
        ListItem(
            modifier = Modifier
                .dpadFocus(RectangleShape)
                .toggleable(
                    value = launchOnConnect,
                    role = Role.Switch,
                    onValueChange = {
                        viewModel.set(Prefs.LAUNCH_ON_CONNECT, it)
                        if (it && !canAutoLaunch(ctx)) ctx.startActivity(_overlayIntent(ctx))
                    }
                ),
            headlineContent = { Text(stringResource(R.string.setting_launch_on_connect)) },
            supportingContent = {
                Text(stringResource(
                    if (needsOverlayPermission) R.string.setting_launch_on_connect_no_permission
                    else R.string.setting_launch_on_connect_desc
                ))
            },
            trailingContent = {
                Switch(checked = launchOnConnect, onCheckedChange = null)
            }
        )

        // 2.1: where the receiver goes when a cast ends on its own (playback finished, or the
        // sender dropped out) rather than by the user pressing back. Staying is what makes the
        // idle QR code reachable after every cast; leaving is the seamless-cast default.
        SettingSegmented(
            title = stringResource(R.string.setting_exit_on_cast_end),
            summary = stringResource(R.string.setting_exit_on_cast_end_desc),
            value = exitOnCastEnd,
            options = listOf(
                false to stringResource(R.string.cast_end_foreground),
                true to stringResource(R.string.cast_end_background),
            ),
            onValueChange = { viewModel.set(Prefs.EXIT_ON_CAST_END, it) }
        )

        SectionHeader(stringResource(R.string.section_display))

        // 2.1: the background is a choice, not a reflection of the TV's own theme — the QR code
        // and every label are derived from the same colour scheme, so they flip with it
        SettingSegmented(
            title = stringResource(R.string.setting_theme),
            summary = stringResource(R.string.setting_theme_desc),
            value = theme,
            options = listOf(
                Prefs.THEME_DARK to stringResource(R.string.theme_dark),
                Prefs.THEME_LIGHT to stringResource(R.string.theme_light),
            ),
            onValueChange = { viewModel.set(Prefs.THEME, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_auto_fullscreen),
            description = stringResource(R.string.setting_auto_fullscreen_desc),
            checked = autoFullscreen,
            onCheckedChange = { viewModel.set(Prefs.AUTO_FULLSCREEN, it) }
        )

        SettingResolution(
            value = resolution,
            onValueChange = { viewModel.set(Prefs.RESOLUTION, it) }
        )

        SettingChipField(
            title = stringResource(R.string.setting_max_fps),
            description = stringResource(R.string.setting_max_fps_desc),
            value = maxFps.toString(),
            presets = listOf("24" to "24", "30" to "30", "60" to "60", "120" to "120"),
            placeholder = stringResource(R.string.setting_max_fps_placeholder),
            keyboardType = KeyboardType.Number,
            onValueChange = { it.toIntOrNull()?.let { v -> viewModel.set(Prefs.MAX_FPS, v) } }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_overscanned),
            description = stringResource(R.string.setting_overscanned_desc),
            checked = overscanned,
            onCheckedChange = { viewModel.set(Prefs.OVERSCANNED, it) }
        )

        // video frames are aligned to the audio playout clock; this nudges the picture
        // relative to it for hardware whose decoded A/V latency differs from the norm
        var avSyncVal by remember(avSyncOffsetMs) { mutableFloatStateOf(avSyncOffsetMs.toFloat()) }
        ListItem(
            headlineContent = {
                Column {
                    Text(stringResource(R.string.setting_av_sync_offset))
                    Slider(
                        value = avSyncVal,
                        onValueChange = { avSyncVal = it },
                        onValueChangeFinished = { viewModel.set(Prefs.AV_SYNC_OFFSET_MS, avSyncVal.roundToInt()) },
                        valueRange = -500f..500f,
                        steps = 19,
                        modifier = Modifier.dpadFocus().dpadAdjust(
                            onLeft = { viewModel.set(Prefs.AV_SYNC_OFFSET_MS, (avSyncOffsetMs - 50).coerceIn(-500, 500)) },
                            onRight = { viewModel.set(Prefs.AV_SYNC_OFFSET_MS, (avSyncOffsetMs + 50).coerceIn(-500, 500)) }
                        )
                    )
                }
            },
            supportingContent = { Text(stringResource(R.string.setting_av_sync_offset_desc)) },
            trailingContent = { Text(stringResource(R.string.av_sync_offset_value, avSyncOffsetMs)) }
        )

        SectionHeader(stringResource(R.string.section_decode))

        SettingSwitch(
            title = stringResource(R.string.setting_h265),
            description = stringResource(R.string.setting_h265_desc),
            checked = h265Enabled,
            onCheckedChange = { viewModel.set(Prefs.H265_ENABLED, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_sw_alac),
            description = stringResource(R.string.setting_sw_alac_desc),
            checked = forceSwAlac,
            onCheckedChange = { viewModel.set(Prefs.FORCE_SW_ALAC, it) }
        )

        SettingSwitch(
            title = stringResource(R.string.setting_sw_aac),
            description = stringResource(R.string.setting_sw_aac_desc),
            checked = forceSwAac,
            onCheckedChange = { viewModel.set(Prefs.FORCE_SW_AAC, it) }
        )

        SectionHeader(stringResource(R.string.section_developer))

        SettingSwitch(
            title = stringResource(R.string.setting_developer_options),
            description = stringResource(R.string.setting_developer_options_desc),
            checked = developerOptions,
            onCheckedChange = { viewModel.set(Prefs.DEVELOPER_OPTIONS, it) }
        )

        if (developerOptions) {
            // the log viewer used to be a bottom-bar tab; it lives here now because it
            // is a diagnostic tool rather than an everyday destination
            if (onOpenLogs != null) {
                ListItem(
                    modifier = Modifier
                        .dpadFocus(RectangleShape)
                        .clickable(onClick = onOpenLogs),
                    headlineContent = { Text(stringResource(R.string.setting_open_logs)) },
                    supportingContent = { Text(stringResource(R.string.setting_open_logs_desc)) }
                )
            }

            SettingSwitch(
                title = stringResource(R.string.setting_auto_start),
                description = stringResource(R.string.setting_auto_start_desc),
                checked = autoStart,
                onCheckedChange = { viewModel.set(Prefs.AUTO_START, it) }
            )

            // draws the dual-pane layout from synthetic panes: with a single phone around
            // there is no other way to check the spacing between two mirror panes
            SettingSegmented(
                title = stringResource(R.string.setting_debug_panes),
                summary = stringResource(R.string.setting_debug_panes_desc),
                value = debugPanes,
                options = listOf(
                    "" to stringResource(R.string.chip_off),
                    "pp" to stringResource(R.string.debug_panes_pp),
                    "pl" to stringResource(R.string.debug_panes_pl),
                    "ipad" to stringResource(R.string.debug_panes_ipad),
                ),
                onValueChange = { viewModel.set(Prefs.DEBUG_PANES, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_keep_screen_on),
                description = stringResource(R.string.setting_keep_screen_on_desc),
                checked = keepScreenOn,
                onCheckedChange = { viewModel.set(Prefs.KEEP_SCREEN_ON, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_idle_preview),
                description = stringResource(R.string.setting_idle_preview_desc),
                checked = idlePreview,
                onCheckedChange = { viewModel.set(Prefs.IDLE_PREVIEW, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_advertise_video),
                description = stringResource(R.string.setting_advertise_video_desc),
                checked = advertiseVideo,
                onCheckedChange = { viewModel.set(Prefs.ADVERTISE_VIDEO, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_advertise_mirroring),
                description = stringResource(R.string.setting_advertise_mirroring_desc),
                checked = advertiseMirroring,
                onCheckedChange = { viewModel.set(Prefs.ADVERTISE_MIRRORING, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_advertise_audio),
                description = stringResource(R.string.setting_advertise_audio_desc),
                checked = advertiseAudio,
                onCheckedChange = { viewModel.set(Prefs.ADVERTISE_AUDIO, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_alac),
                description = stringResource(R.string.setting_alac_desc),
                checked = alacEnabled,
                onCheckedChange = { viewModel.set(Prefs.ALAC_ENABLED, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_aac),
                description = stringResource(R.string.setting_aac_desc),
                checked = aacEnabled,
                onCheckedChange = { viewModel.set(Prefs.AAC_ENABLED, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_key_allow_frame_drop),
                description = stringResource(R.string.setting_key_allow_frame_drop_desc),
                checked = keyAllowFrameDrop,
                onCheckedChange = { viewModel.set(Prefs.KEY_ALLOW_FRAME_DROP, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_enforce_sdr),
                description = stringResource(R.string.setting_enforce_sdr_desc),
                checked = enforceSdr,
                onCheckedChange = { viewModel.set(Prefs.ENFORCE_SDR, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_realtime_decoder_priority),
                description = stringResource(R.string.setting_realtime_decoder_priority_desc),
                checked = realtimeDecoderPriority,
                onCheckedChange = { viewModel.set(Prefs.KEY_PRIORITY, it) }
            )

            SettingChips(
                title = stringResource(R.string.setting_operating_rate),
                description = stringResource(R.string.setting_operating_rate_desc),
                value = operatingRate,
                options = listOf(
                    Prefs.AUTO to stringResource(R.string.chip_auto),
                    Prefs.ON to stringResource(R.string.chip_on),
                    Prefs.OFF to stringResource(R.string.chip_off),
                ),
                onValueChange = { viewModel.set(Prefs.OPERATING_RATE, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_low_latency),
                description = stringResource(R.string.setting_low_latency_desc),
                checked = lowLatency,
                onCheckedChange = { viewModel.set(Prefs.LOW_LATENCY, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_scheduled_output_buffer_release),
                description = stringResource(R.string.setting_scheduled_output_buffer_release_desc),
                checked = scheduledOutputBufferRelease,
                onCheckedChange = { viewModel.set(Prefs.SCHEDULED_OUTPUT_BUFFER_RELEASE, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_audio_delay),
                description = stringResource(R.string.setting_audio_delay_desc),
                checked = audioLatencyMs >= 0,
                onCheckedChange = { viewModel.set(Prefs.AUDIO_LATENCY_MS, if (it) 250 else -1) }
            )

            if (audioLatencyMs >= 0) {
                var sliderVal by remember(audioLatencyMs) { mutableFloatStateOf(audioLatencyMs.toFloat()) }
                ListItem(
                    headlineContent = {
                        Slider(
                            value = sliderVal,
                            onValueChange = { sliderVal = it },
                            onValueChangeFinished = { viewModel.set(Prefs.AUDIO_LATENCY_MS, sliderVal.roundToInt()) },
                            valueRange = 0f..1000f,
                            steps = 19,
                            modifier = Modifier.dpadFocus().dpadAdjust(
                                onLeft = { viewModel.set(Prefs.AUDIO_LATENCY_MS, (audioLatencyMs - 50).coerceIn(0, 1000)) },
                                onRight = { viewModel.set(Prefs.AUDIO_LATENCY_MS, (audioLatencyMs + 50).coerceIn(0, 1000)) }
                            )
                        )
                    },
                    trailingContent = { Text(stringResource(R.string.audio_delay_value, sliderVal.roundToInt())) }
                )
            }

            SettingSwitch(
                title = stringResource(R.string.setting_audio_auto_buffer),
                description = stringResource(R.string.setting_audio_auto_buffer_desc),
                checked = audioAutoBuffer,
                onCheckedChange = { viewModel.set(Prefs.AUDIO_AUTO_BUFFER, it) }
            )

            if (audioAutoBuffer) {
                val maxStep = Prefs.AUDIO_ADAPTIVE_STEP.range.last
                var stepVal by remember(audioAdaptiveStep) { mutableFloatStateOf(audioAdaptiveStep.toFloat()) }
                ListItem(
                    headlineContent = {
                        Slider(
                            value = stepVal,
                            onValueChange = { stepVal = it },
                            onValueChangeFinished = { viewModel.set(Prefs.AUDIO_ADAPTIVE_STEP, stepVal.roundToInt()) },
                            valueRange = 0f..maxStep.toFloat(),
                            steps = maxStep - 1,
                            modifier = Modifier.dpadFocus().dpadAdjust(
                                onLeft = { viewModel.set(Prefs.AUDIO_ADAPTIVE_STEP, (audioAdaptiveStep - 1).coerceIn(0, maxStep)) },
                                onRight = { viewModel.set(Prefs.AUDIO_ADAPTIVE_STEP, (audioAdaptiveStep + 1).coerceIn(0, maxStep)) }
                            )
                        )
                    },
                    trailingContent = {
                        val step = stepVal.roundToInt().coerceIn(0, maxStep)
                        Text(stringResource(R.string.audio_adaptive_value,
                            Prefs.ADAPTIVE_PERCENTILES[step],
                            stringArrayResource(R.array.audio_adaptive_step_names)[step]))
                    }
                )
            } else {
                SettingTextField(
                    label = stringResource(R.string.setting_audio_cushion_ms),
                    value = audioCushionMs.toString(),
                    onCommit = { viewModel.set(Prefs.AUDIO_CUSHION_MS, it.toInt()) },
                    description = stringResource(R.string.setting_audio_cushion_ms_desc),
                    range = Prefs.AUDIO_CUSHION_MS.range
                )
            }

            SettingTextField(
                label = stringResource(R.string.setting_oboe_buffer_frames),
                value = oboeBufferFrames.toString(),
                onCommit = { viewModel.set(Prefs.OBOE_BUFFER_FRAMES, it.toInt()) },
                description = stringResource(R.string.setting_oboe_buffer_frames_desc),
                range = Prefs.OBOE_BUFFER_FRAMES.range
            )


            SettingSwitch(
                title = stringResource(R.string.setting_debug_overlay),
                description = stringResource(R.string.setting_debug_overlay_desc),
                checked = debugEnabled,
                onCheckedChange = { viewModel.set(Prefs.DEBUG_ENABLED, it) }
            )

            SettingSwitch(
                title = stringResource(R.string.setting_benchmark_log),
                description = stringResource(R.string.setting_benchmark_log_desc),
                checked = benchmarkLog,
                onCheckedChange = { viewModel.set(Prefs.BENCHMARK_LOG, it) }
            )
        }

        SectionHeader(stringResource(R.string.section_about))

        var showAbout by remember { mutableStateOf(false) }
        val ctx2 = LocalContext.current
        val pkgInfo = remember { runCatching { ctx2.packageManager.getPackageInfo(ctx2.packageName, 0) }.getOrNull() }
        ListItem(
            modifier = Modifier
                .dpadFocus(RectangleShape)
                .clickable { showAbout = true },
            headlineContent = { Text(stringResource(R.string.setting_about)) },
            supportingContent = { Text(stringResource(R.string.setting_about_desc)) }
        )
        if (showAbout) {
            AlertDialog(
                onDismissRequest = { showAbout = false },
                title = { Text(stringResource(R.string.app_name)) },
                text = {
                    Column {
                        Text(stringResource(R.string.about_version, pkgInfo?.versionName ?: "?", pkgInfo?.versionCode ?: 0))
                        Spacer(Modifier.height(8.dp))
                        Text(stringResource(R.string.about_copyright))
                    }
                },
                confirmButton = {
                    TextButton(
                        onClick = { showAbout = false },
                        modifier = Modifier.dpadFocus(RectangleShape)
                    ) { Text(stringResource(R.string.btn_ok)) }
                }
            )
        }

        // the only way out: back leaves the screen instead of killing the receiver, so a real
        // quit has to be asked for explicitly (and confirmed, it drops a live cast)
        if (onQuit != null) {
            var showQuit by remember { mutableStateOf(false) }
            ListItem(
                modifier = Modifier
                    .dpadFocus(RectangleShape)
                    .clickable { showQuit = true },
                headlineContent = { Text(stringResource(R.string.setting_quit)) },
                supportingContent = { Text(stringResource(R.string.setting_quit_desc)) }
            )
            if (showQuit) {
                AlertDialog(
                    onDismissRequest = { showQuit = false },
                    title = { Text(stringResource(R.string.setting_quit)) },
                    text = { Text(stringResource(R.string.setting_quit_confirm)) },
                    confirmButton = {
                        TextButton(
                            onClick = {
                                showQuit = false
                                onQuit()
                            },
                            modifier = Modifier.dpadFocus(RectangleShape)
                        ) { Text(stringResource(R.string.setting_quit)) }
                    },
                    dismissButton = {
                        TextButton(
                            onClick = { showQuit = false },
                            modifier = Modifier.dpadFocus(RectangleShape)
                        ) { Text(stringResource(R.string.btn_cancel)) }
                    }
                )
            }
        }
        }
    }
}

private fun canAutoLaunch(ctx: Context): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || Settings.canDrawOverlays(ctx)

private fun _overlayIntent(ctx: Context): Intent =
    Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:${ctx.packageName}"))
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

@Composable
private fun SectionHeader(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp)
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun SettingResolution(
    value: String,
    onValueChange: (String) -> Unit
) {
    val presets = listOf(
        Prefs.AUTO to stringResource(R.string.chip_auto),
        "1280x720" to "1280x720",
        "1920x1080" to "1920x1080",
        "3840x2160" to "3840x2160"
    )
    val devicePresets = listOf(
        Prefs.PORTRAIT to stringResource(R.string.chip_device_portrait),
        Prefs.LANDSCAPE to stringResource(R.string.chip_device_landscape)
    )
    val isPreset = (presets + devicePresets).any { it.first == value }
    var editing by remember { mutableStateOf(false) }
    val parts = if (!isPreset && value.contains("x")) value.split("x", limit = 2) else listOf("", "")
    var width by remember(value) { mutableStateOf(if (isPreset) "" else parts[0]) }
    var height by remember(value) { mutableStateOf(if (isPreset) "" else parts.getOrElse(1) { "" }) }

    ListItem(
        headlineContent = { Text(stringResource(R.string.setting_resolution)) },
        supportingContent = {
            Column {
                Text(stringResource(R.string.setting_resolution_desc))
                Spacer(Modifier.height(8.dp))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    @Composable
                    fun presetChip(key: String, label: String) = FilterChip(
                        selected = value == key && !editing,
                        onClick = {
                            editing = false
                            width = ""; height = ""
                            onValueChange(key)
                        },
                        label = { Text(label) },
                        modifier = Modifier.dpadFocus()
                    )
                    presets.forEach { (key, label) -> presetChip(key, label) }
                    FilterChip(
                        selected = !isPreset || editing,
                        onClick = { editing = true },
                        label = { Text(stringResource(R.string.chip_custom)) },
                        modifier = Modifier.dpadFocus()
                    )
                    devicePresets.forEach { (key, label) -> presetChip(key, label) }
                }
                if (editing || !isPreset) {
                    Spacer(Modifier.height(8.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = width,
                            onValueChange = { width = it.filter { c -> c.isDigit() }.take(5) },
                            singleLine = true,
                            label = { Text(stringResource(R.string.setting_resolution_width)) },
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Number,
                                imeAction = ImeAction.Next
                            ),
                            modifier = Modifier.weight(1f)
                        )
                        OutlinedTextField(
                            value = height,
                            onValueChange = { height = it.filter { c -> c.isDigit() }.take(5) },
                            singleLine = true,
                            label = { Text(stringResource(R.string.setting_resolution_height)) },
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Number,
                                imeAction = ImeAction.Done
                            ),
                            keyboardActions = doneAndHide {
                                val w = width.toIntOrNull()
                                val h = height.toIntOrNull()
                                if (w != null && w > 0 && h != null && h > 0) {
                                    onValueChange("${w}x${h}")
                                    editing = false
                                }
                            },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
            }
        }
    )
}

@Composable
private fun doneAndHide(commit: () -> Unit): KeyboardActions {
    val keyboard = LocalSoftwareKeyboardController.current
    return KeyboardActions(onDone = { commit(); keyboard?.hide() })
}

@Composable
private fun SettingTextField(
    label: String,
    value: String,
    onCommit: (String) -> Unit,
    description: String? = null,
    range: IntRange? = null
) {
    // TV/remote input: moving focus to the field must only SELECT it (highlight), never pop the
    // on-screen keyboard, so the user can keep navigating with the d-pad. The keyboard appears
    // only when the user presses the confirm key, which flips to an edit mode that focuses a
    // real TextField (whose IME request then shows the keyboard). This two-state split is the
    // only reliable way to stop the TextField's own focus from auto-requesting the IME.
    var editing by remember(value) { mutableStateOf(false) }
    var focused by remember { mutableStateOf(false) }
    val editFocus = remember { FocusRequester() }

    if (editing) {
        // start with the caret at the END of the existing value so the user can immediately
        // backspace/delete the current text without first moving the cursor from the start
        var text by remember(value) {
            mutableStateOf(TextFieldValue(value, selection = TextRange(value.length)))
        }
        val valid = range == null || text.text.toIntOrNull()?.let { it in range } == true
        // true only after this TextField has actually held focus at least once. Without it, the
        // initial onFocusChanged(false) that fires before the field grabs focus would immediately
        // drop us back out of edit mode, so tapping/pressing confirm would appear to do nothing.
        var everFocused by remember { mutableStateOf(false) }
        fun save() {
            val s = text.text
            if (valid && s != value) onCommit(s)
        }
        // focus the edit field once it appears so its IME pops
        val editKeyboard = LocalSoftwareKeyboardController.current
        LaunchedEffect(Unit) {
            editFocus.requestFocus()
            // let focus land on the text field, then force-show the IME (some devices don't pop
            // the keyboard from requestFocus() alone)
            delay(60)
            editKeyboard?.show()
        }
        OutlinedTextField(
            value = text,
            onValueChange = { newVal ->
                text = if (range == null) newVal
                       else newVal.copy(text = newVal.text.filter { c -> c.isDigit() }.take(6))
            },
            label = { Text(label) },
            supportingText = description?.let { { Text(it) } },
            singleLine = true,
            isError = !valid,
            keyboardOptions = KeyboardOptions(
                keyboardType = if (range == null) KeyboardType.Text else KeyboardType.Number,
                imeAction = ImeAction.Done
            ),
            keyboardActions = doneAndHide {
                save()
                editing = false
            },
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .padding(bottom = 8.dp)
                .focusRequester(editFocus)
                .onFocusChanged { state ->
                    if (state.isFocused) {
                        everFocused = true
                    } else if (everFocused) {
                        // left edit mode (d-pad moved on, back, or clicked away) -> commit
                        editing = false
                        save()
                    }
                }
        )
    } else {
        Surface(
            shape = MaterialTheme.shapes.medium,
            color = if (focused) MaterialTheme.colorScheme.primaryContainer
                    else MaterialTheme.colorScheme.surfaceVariant,
            border = BorderStroke(
                if (focused) 2.dp else 1.dp,
                if (focused) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant
            ),
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .padding(bottom = 8.dp)
                .clickable { editing = true }
                .focusable()
                .onFocusChanged { state -> focused = state.isFocused }
                .onKeyEvent { ev ->
                    // TV remote: make the confirm key explicitly activate edit mode. Some remote
                    // stacks don't route DPAD_CENTER/Enter through the clickable semantic, so we
                    // handle it here to guarantee it works regardless of focus/click plumbing.
                    if (ev.type == KeyEventType.KeyDown &&
                        (ev.key == Key.Enter || ev.key == Key.NumPadEnter || ev.key == Key.DirectionCenter)
                    ) {
                        editing = true
                        true
                    } else {
                        false
                    }
                }
        ) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
                Text(
                    label,
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    if (value.isBlank()) label else value,
                    style = MaterialTheme.typography.bodyLarge,
                    color = if (focused) MaterialTheme.colorScheme.onPrimaryContainer
                            else MaterialTheme.colorScheme.onSurface
                )
                description?.let {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingChipField(
    title: String,
    description: String,
    value: String,
    presets: List<Pair<String, String>>,
    placeholder: String,
    keyboardType: KeyboardType = KeyboardType.Text,
    onValueChange: (String) -> Unit
) {
    val isPreset = presets.any { it.first == value }
    var editing by remember { mutableStateOf(false) }
    var text by remember(value) { mutableStateOf(if (isPreset) "" else value) }

    ListItem(
        headlineContent = { Text(title) },
        supportingContent = {
            Column {
                Text(description)
                Spacer(Modifier.height(8.dp))
                ChipRow(if (editing) "" else value, presets, { editing = false; text = ""; onValueChange(it) }) {
                    FilterChip(
                        selected = !isPreset || editing,
                        onClick = { editing = true },
                        label = { Text(stringResource(R.string.chip_custom)) },
                        modifier = Modifier.dpadFocus()
                    )
                }
                if (editing || !isPreset) {
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = text,
                        onValueChange = { text = it },
                        singleLine = true,
                        label = { Text(placeholder) },
                        keyboardOptions = KeyboardOptions(
                            keyboardType = keyboardType,
                            imeAction = ImeAction.Done
                        ),
                        keyboardActions = doneAndHide {
                            if (text.isNotBlank()) {
                                onValueChange(text)
                                editing = false
                            }
                        },
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }
        }
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ChipRow(
    value: String,
    options: List<Pair<String, String>>,
    onSelect: (String) -> Unit,
    trailing: @Composable () -> Unit = {}
) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        options.forEach { (key, label) ->
            FilterChip(
                selected = value == key,
                onClick = { onSelect(key) },
                label = { Text(label) },
                modifier = Modifier.dpadFocus()
            )
        }
        trailing()
    }
}

@Composable
private fun SettingChips(
    title: String,
    description: String,
    value: String,
    options: List<Pair<String, String>>,
    onValueChange: (String) -> Unit
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = {
            Column {
                Text(description)
                Spacer(Modifier.height(8.dp))
                ChipRow(value, options, onValueChange)
            }
        }
    )
}

@Composable
private fun <T> SettingSegmented(
    title: String,
    summary: String,
    value: T,
    options: List<Pair<T, String>>,
    onValueChange: (T) -> Unit
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = {
            Column {
                Spacer(Modifier.height(8.dp))
                SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                    options.forEachIndexed { i, (key, label) ->
                        val shape = SegmentedButtonDefaults.itemShape(i, options.size)
                        SegmentedButton(
                            selected = value == key,
                            onClick = { onValueChange(key) },
                            shape = shape,
                            modifier = Modifier.dpadFocus(shape),
                            label = { Text(label) }
                        )
                    }
                }
                Spacer(Modifier.height(8.dp))
                Text(summary)
            }
        }
    )
}

@Composable
private fun SettingSwitch(
    title: String,
    description: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    ListItem(
        modifier = Modifier
            .dpadFocus(RectangleShape)
            .toggleable(value = checked, role = Role.Switch, onValueChange = onCheckedChange),
        headlineContent = { Text(title) },
        supportingContent = { Text(description) },
        trailingContent = { Switch(checked = checked, onCheckedChange = null) }
    )
}
