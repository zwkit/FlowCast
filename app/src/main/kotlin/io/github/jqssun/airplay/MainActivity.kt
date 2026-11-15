package io.github.jqssun.airplay

import android.Manifest
import android.app.AlertDialog
import android.app.PictureInPictureParams
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.util.Rational
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import io.github.jqssun.airplay.service.AirPlayService
import io.github.jqssun.airplay.ui.MainScreen
import io.github.jqssun.airplay.ui.isTvDevice
import io.github.jqssun.airplay.ui.theme.AirPlayTheme
import io.github.jqssun.airplay.viewmodel.LocalNetworkAccess
import io.github.jqssun.airplay.viewmodel.MainViewModel
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(Locales.apply(newBase))
    }

    override fun getResources(): android.content.res.Resources =
        Locales.localized(super.getResources(), this)

    private val viewModel: MainViewModel by viewModels()
    private var service: AirPlayService? = null
    val isInPip = mutableStateOf(false)
    private val logCallback: (String) -> Unit = { viewModel.addLog(it) }
    private val pinCallback: (String?) -> Unit = { viewModel.showPin(it) }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val svc = (binder as? AirPlayService.LocalBinder)?.service ?: return
            service = svc
            svc.logCallback = logCallback
            svc.pinCallback = pinCallback
            viewModel.bindService(svc)
            if (lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) _refreshForeground()
            _autoStart()
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            viewModel.unbindService()
        }
    }

    private val permissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { if (it.isNotEmpty()) _syncLocalNetwork(requested = true) }

    private var lastBackPressAt = 0L
    private val prefs by lazy { getSharedPreferences(Prefs.NAME, Context.MODE_PRIVATE) }
    // set when the settings page's back press returned home: the very next back press
    // (inside SETTINGS_BACK_EXIT_WINDOW_MS) backgrounds right away; once the window lapses the
    // usual double-press applies
    private var lastSettingsBackAt = 0L

    /** the settings page's back press returned to the home page */
    fun onSettingsBack() {
        lastSettingsBackAt = SystemClock.elapsedRealtime()
    }

    override fun onBackPressed() {
        // Compose BackHandlers (settings page, fullscreen, logs, speed selector) must get the
        // first chance. On API 33+ the platform callback reaches the dispatcher directly, but on
        // older releases (the TV runs Android 12) the framework calls THIS method first, so the
        // delegation has to happen here or those screens would swallow the exit toast instead.
        if (onBackPressedDispatcher.hasEnabledCallbacks()) {
            super.onBackPressed()
            return
        }
        val now = SystemClock.elapsedRealtime()
        // while casting, the first back press only hints; the second one (within 2s) ends the
        // cast and moves the whole task to the background — no return to the preview UI — so the
        // receiver keeps running seamlessly (the foreground service stays up in the background).
        if (viewModel.isCastingActive()) {
            if (now - lastBackPressAt < 2000) {
                lastBackPressAt = 0
                viewModel.disconnectCast()
                moveTaskToBack(true)
            } else {
                lastBackPressAt = now
                Toast.makeText(this, R.string.press_back_again_to_exit_cast, Toast.LENGTH_SHORT).show()
            }
            return
        }
        // Back never quits the receiver: this is an always-on box, and a quit that takes the
        // service down means the device silently stops accepting casts until someone walks over
        // and taps the icon again. Both branches below only leave the screen.
        // just came back from the settings page: a single press leaves
        if (now - lastSettingsBackAt < SETTINGS_BACK_EXIT_WINDOW_MS) {
            lastSettingsBackAt = 0
            lastBackPressAt = 0
            _moveToBackground()
            return
        }
        // not casting: double back-press leaves the screen (first press shows a hint)
        if (now - lastBackPressAt < 2000) {
            lastBackPressAt = 0
            _moveToBackground()
        } else {
            lastBackPressAt = now
            Toast.makeText(this, R.string.press_back_again_to_exit, Toast.LENGTH_SHORT).show()
        }
    }

    /** Leaves the screen but keeps the receiver alive, still listening for the next cast. */
    private fun _moveToBackground() {
        if (isFinishing || isDestroyed) return
        moveTaskToBack(true)
    }

    /**
     * Settings -> About -> Quit: the one and only way to actually stop the receiver.
     * Back no longer does this (an all-in-one receiver that a stray back press can kill is a
     * receiver nobody can rely on), so quitting is an explicit, confirmed action instead.
     */
    fun quitApp() {
        runCatching { viewModel.disconnectCast() }
        _stopReceiverService()
        finishAndRemoveTask()
    }

    /**
     * Finishing the activity on its own left the foreground service running, still listening on
     * port 7000 with the sender's TCP connection intact. Stopping the server has to be explicit:
     * that is what [quitApp] does. The service is started again on the next launch, so stopping
     * it only ends the current run.
     */
    private fun _stopReceiverService() {
        runCatching { stopService(Intent(this, AirPlayService::class.java)) }
    }

    /**
     * True when this launch must NOT show the home page: the app is meant to run in the
     * background (boot auto-start, launcher boot auto-start, any other non-user launch).
     * Only an explicit user action — a launcher tap or a cast/notification — shows the UI.
     */
    private fun _shouldStayInBackground(intent: Intent?): Boolean {
        if (intent == null) return false
        // a cast (someone connects and the receiver must show it) or a notification tap
        if (intent.getBooleanExtra(EXTRA_FROM_CAST, false)) return false
        if (intent.getBooleanExtra(EXTRA_FROM_NOTIFICATION, false)) return false
        // Everything else is a user launch (launcher tap, recents, "open app" from settings).
        // The one exception is the launcher's own auto-start, which fires left, right after the
        // system comes up: that launch must only bring the receiver up in the background.
        // Timing is used rather than any "who started me" hint because the TV launcher's
        // auto-start drops every category/extra we could key on.
        //
        // Only the FIRST launch in that window is swallowed. Swallowing all of them made a
        // person who turned the TV on and tapped the icon seconds later stare at nothing, which
        // reads as a broken app; one auto-start per boot is all we need to hide.
        val now = SystemClock.elapsedRealtime()
        if (now >= BOOT_SILENT_WINDOW_MS) return false
        val lastSilentAt = prefs.getLong(KEY_LAST_SILENT_LAUNCH_AT, -1L)
        // elapsedRealtime restarts at 0 on boot, so a marker far in the "future" means the device
        // rebooted since it was written - exactly the case that must stay silent again
        if (lastSilentAt in 0..now) return false
        prefs.edit().putLong(KEY_LAST_SILENT_LAUNCH_AT, now).apply()
        return true
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // decide before any UI is composed: a non-user launch must not flash the home page
        if (_shouldStayInBackground(intent)) {
            // keep the receiver available in the background: with an auto-start preference the
            // server comes up silently (it usually already runs — the boot receiver started it)
            val prefs = getSharedPreferences(Prefs.NAME, Context.MODE_PRIVATE)
            val autoStart = prefs[Prefs.AUTO_START] || prefs[Prefs.BOOT_AUTO_START]
            val svc = Intent(this, AirPlayService::class.java)
            if (autoStart) {
                svc.action = AirPlayService.ACTION_START_SERVER
                ContextCompat.startForegroundService(this, svc)
            } else {
                startService(svc)
            }
            finish()
            runCatching { overridePendingTransition(0, 0) }
            return
        }
        enableEdgeToEdge()
        Locales.applySystemLocale(this)
        _maybePromptOverlayPermission()
        val missing = buildList {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !_granted(Manifest.permission.POST_NOTIFICATIONS)) {
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
            if (!hasLocalNetwork()) add(Manifest.permission.ACCESS_LOCAL_NETWORK)
        }
        if (missing.isNotEmpty()) permissions.launch(missing.toTypedArray())

        bindService(Intent(this, AirPlayService::class.java), connection, BIND_AUTO_CREATE)

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (true) {
                    viewModel.updateFromService()
                    delay(200)
                }
            }
        }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                combine(viewModel.pref(Prefs.KEEP_SCREEN_ON), viewModel.connectionCount) { keep, conns ->
                    keep && conns > 0
                }.collect { on ->
                    if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            }
        }

        // seamless cast: when a phone-initiated cast session ends (the sender disconnects on its
        // own, or playback finishes), drop straight to the background instead of showing the home
        // UI, so the app is as invisible as possible. Back-key teardown already backgrounds it, so
        // this only fires for casts that end without the TV user pressing back.
        //
        // Prefs.EXIT_ON_CAST_END turns that off: some setups want the receiver to come back to
        // its own home page (which since 2.1 is where the "push a video" QR code lives) instead
        // of vanishing. The double-back exit stays a deliberate gesture and always backgrounds.
        //
        // Two guards, both earned from a takeover that went wrong (an AirPlay mirror replacing a
        // running DLNA video pushed the app to the background, so the new mirror rendered into a
        // surface nobody could see):
        //  - the flag is a single atomically-published value (viewModel.castActive), so the
        //    hand-over between two sessions is never observed as "nothing is casting";
        //  - the end has to *hold* for CAST_END_GRACE_MS before the app leaves. A takeover, a
        //    re-negotiated stream or a codec restart all dip through "idle" for a few hundred ms,
        //    and leaving on that instant is what hid the new session.
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                var wasCasting = false
                var endJob: Job? = null
                viewModel.castActive.collect { casting ->
                    if (casting) {
                        endJob?.cancel()
                        endJob = null
                        wasCasting = true
                    } else if (wasCasting && endJob == null) {
                        endJob = launch {
                            delay(CAST_END_GRACE_MS)
                            endJob = null
                            wasCasting = false
                            // read the preference at decision time, not when the watch was
                            // armed: a setting flipped during those few seconds of grace has
                            // to count. Prefs.EXIT_ON_CAST_END off = stay on the home page
                            if (!isFinishing && !isDestroyed &&
                                viewModel.pref(Prefs.EXIT_ON_CAST_END).value
                            ) {
                                moveTaskToBack(true)
                            }
                        }
                    }
                }
            }
        }

        // TV-only: the home page must never sit in the task stack. When the activity stops
        // without a live cast (screen off, HOME, app switch) it finishes itself, so turning
        // the TV back on shows the launcher instead of restoring the app — the user gets the
        // home page only by opening the app manually (or when someone casts).
        if (isTvDevice()) {
            // the activity can also be stopped while a cast is live; when that cast later ends
            // there is no new onStop to react to, so watch for it here (this collector is not
            // lifecycle-bound: it stays alive while the activity is stopped). drop(1) skips the
            // current value the flow replays on subscribe — reacting to that would finish the
            // activity during its own cold start.
            lifecycleScope.launch {
                viewModel.castActive.drop(1).collect { casting ->
                    if (!casting && !isFinishing && !isChangingConfigurations &&
                        !lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) &&
                        !isInPip.value
                    ) {
                        finish()
                    }
                }
            }
        }

        // auto-enter pre-declared in pip params
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            lifecycleScope.launch {
                repeatOnLifecycle(Lifecycle.State.STARTED) {
                    combine(
                        viewModel.serverState, viewModel.connectionCount,
                        viewModel.videoPlaybackActive, viewModel.videoPlaybackAspect,
                        viewModel.videoAspect
                    ) { _, _, _, _, _ -> }.collect { setPictureInPictureParams(_pipParams()) }
                }
            }
        }

        setContent {
            // 2.1: the background is an explicit setting now, no longer "whatever the TV happens
            // to be set to" — a receiver that changes colour with the TV's theme reads as broken
            val theme by viewModel.pref(Prefs.THEME).collectAsState()
            val darkTheme = theme != Prefs.THEME_LIGHT
            // enableEdgeToEdge() decided the bar icon colour from the *system* theme, which can
            // disagree with this one: a light app on a dark TV would get white icons on white
            LaunchedEffect(darkTheme) {
                WindowCompat.getInsetsController(window, window.decorView).run {
                    isAppearanceLightStatusBars = !darkTheme
                    isAppearanceLightNavigationBars = !darkTheme
                }
            }
            // Inject an explicitly localized context so Compose's stringResource()
            // resolves the selected app language regardless of the system override
            // of getResources()/updateConfiguration() on modern Android.
            val localizedContext = androidx.compose.runtime.remember { Locales.apply(this) }
            androidx.compose.runtime.CompositionLocalProvider(
                androidx.compose.ui.platform.LocalContext provides localizedContext
            ) {
                AirPlayTheme(darkTheme = darkTheme) {
                    MainScreen(
                        viewModel = viewModel,
                        isInPip = isInPip.value,
                        onSurfaceAvailable = { viewModel.onSurfaceAvailable(it) },
                        onSurfaceDestroyed = { viewModel.onSurfaceDestroyed(it) },
                        onPip = { enterPip() },
                        onLocalNetwork = ::_requestLocalNetwork,
                        onSettingsBack = ::onSettingsBack,
                        onQuit = ::quitApp
                    )
                }
            }
        }
    }

    private fun _maybePromptOverlayPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        if (!viewModel.pref(Prefs.LAUNCH_ON_CONNECT).value) return
        if (Settings.canDrawOverlays(this)) return
        val prefs = getSharedPreferences(Prefs.NAME, Context.MODE_PRIVATE)
        if (prefs.getBoolean("overlay_prompt_shown", false)) return
        prefs.edit().putBoolean("overlay_prompt_shown", true).apply()
        // delay past the Android 13+ locale recreate so the dialog is not torn down
        Handler(Looper.getMainLooper()).postDelayed({
            if (isFinishing || isDestroyed) return@postDelayed
            AlertDialog.Builder(this)
                .setTitle(getString(R.string.overlay_permission_title))
                .setMessage(getString(R.string.overlay_permission_message))
                .setPositiveButton(getString(R.string.btn_open_settings)) { _, _ ->
                    runCatching {
                        startActivity(
                            Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName"))
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        )
                    }
                }
                .setNegativeButton(getString(R.string.btn_later), null)
                .show()
        }, 600L)
    }

    private fun _granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    // not private: AirPlayService references the launch extras below
    companion object {
        // how long "nothing is casting" has to hold before the app leaves the screen; one
        // 200 ms poll plus a session hand-over never lasts this long, a real end always does
        const val CAST_END_GRACE_MS = 1500L
        // after returning from the settings page, one more back press inside this window exits
        const val SETTINGS_BACK_EXIT_WINDOW_MS = 2000L
        // a launch this soon after the system booted is the launcher's auto-start, not a person
        const val BOOT_SILENT_WINDOW_MS = 90_000L
        // elapsedRealtime of the auto-start launch we already swallowed for this boot
        private const val KEY_LAST_SILENT_LAUNCH_AT = "last_silent_launch_at"
        // launch extras set by the service / notification so the UI knows it may show itself
        const val EXTRA_FROM_CAST = "io.github.jqssun.airplay.extra.FROM_CAST"
        const val EXTRA_FROM_NOTIFICATION = "io.github.jqssun.airplay.extra.FROM_NOTIFICATION"
    }

    private fun _autoStart() {
        val svc = service ?: return
        if (viewModel.pref(Prefs.AUTO_START).value && svc.serverState.value == AirPlayService.ServerState.STOPPED && hasLocalNetwork()) {
            viewModel.startServer()
        }
    }
    
    private fun _refreshForeground() {
        if (service?.serverState?.value == AirPlayService.ServerState.RUNNING) {
            startService(Intent(this, AirPlayService::class.java))
        }
    }

    private fun _syncLocalNetwork(requested: Boolean = false) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.CINNAMON_BUN) return
        val p = Manifest.permission.ACCESS_LOCAL_NETWORK
        val prev = viewModel.localNetwork.value
        val now = when {
            hasLocalNetwork() -> LocalNetworkAccess.GRANTED
            shouldShowRequestPermissionRationale(p) -> LocalNetworkAccess.ASK
            requested || prev == LocalNetworkAccess.SETTINGS -> LocalNetworkAccess.SETTINGS
            else -> LocalNetworkAccess.ASK
        }
        viewModel.setLocalNetwork(now)
        if (now == LocalNetworkAccess.GRANTED && prev != LocalNetworkAccess.GRANTED) _autoStart()
    }

    private fun _requestLocalNetwork() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.CINNAMON_BUN) return
        if (viewModel.localNetwork.value == LocalNetworkAccess.SETTINGS) {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null)))
        } else {
            permissions.launch(arrayOf(Manifest.permission.ACCESS_LOCAL_NETWORK))
        }
    }

    override fun onStart() {
        super.onStart()
        _syncLocalNetwork()
        _refreshForeground()
    }

    fun enterPip() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        enterPictureInPictureMode(_pipParams())
    }

    private fun _pipParams(): PictureInPictureParams {
        val aspect = if (viewModel.videoPlaybackActive.value) viewModel.videoPlaybackAspect.value
            else viewModel.videoAspect.value
        val rational = Rational((aspect * 1000).toInt().coerceIn(1, 2390), 1000)
        val builder = PictureInPictureParams.Builder().setAspectRatio(rational)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            builder.setAutoEnterEnabled(_shouldAutoPip())
        }
        return builder.build()
    }

    private fun _shouldAutoPip(): Boolean =
        viewModel.serverState.value == AirPlayService.ServerState.RUNNING &&
            viewModel.connectionCount.value > 0

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S && _shouldAutoPip()) enterPip()
    }

    override fun onPictureInPictureModeChanged(inPip: Boolean, newConfig: Configuration) {
        super.onPictureInPictureModeChanged(inPip, newConfig)
        isInPip.value = inPip
    }

    override fun onStop() {
        super.onStop()
        // never stop mid-session
        if (!viewModel.pref(Prefs.RUN_IN_BACKGROUND).value && !isChangingConfigurations &&
            viewModel.serverState.value == AirPlayService.ServerState.RUNNING &&
            viewModel.connectionCount.value == 0) {
            viewModel.stopServer()
        }
        // TV: without a live cast the home page is not worth keeping in the task stack —
        // finishing here is what stops the TV from restoring it when the screen comes back
        if (isTvDevice() && !isFinishing && !isChangingConfigurations && !isInPip.value &&
            !viewModel.isCastingActive()
        ) {
            finish()
        }
    }

    override fun onDestroy() {
        // leaving the activity tears down any live cast but keeps the service running
        if (viewModel.isCastingActive()) viewModel.disconnectCast()
        service?.let {
            if (it.logCallback === logCallback) it.logCallback = null
            if (it.pinCallback === pinCallback) it.pinCallback = null
        }
        unbindService(connection)
        super.onDestroy()
    }
}
