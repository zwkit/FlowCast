package io.github.jqssun.airplay

import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.content.res.Resources
import android.os.Build
import android.os.LocaleList
import java.util.Locale

/**
 * Per-app language handling (no appcompat / AppCompatDelegate needed).
 * The selected tag is stored in [Prefs.APP_LANGUAGE] and defaults to
 * Simplified Chinese (zh-rCN).
 *
 * Three cooperating mechanisms guarantee the UI language across Android
 * versions:
 *  - [applySystemLocale]: Android 13+ uses the system per-app LocaleManager
 *    (required there; updateConfiguration is overridden by the system).
 *  - [apply]: wraps the base context (used from attachBaseContext).
 *  - [localized]: forces any [Resources] to the selected language (Android 12
 *    and below, where updateConfiguration still applies).
 */
object Locales {
    const val SYSTEM = "system"
    const val ZH = "zh-rCN"
    const val EN = "en"

    fun currentTag(context: Context): String {
        val prefs = context.getSharedPreferences(Prefs.NAME, Context.MODE_PRIVATE)
        return prefs.getString(Prefs.APP_LANGUAGE.key, Prefs.APP_LANGUAGE.def)
            ?: Prefs.APP_LANGUAGE.def
    }

    fun currentLocale(tag: String): Locale =
        if (tag == SYSTEM) Locale.getDefault()
        else Locale.forLanguageTag(tag.replace("-r", "-")) // zh-rCN -> zh-CN

    /**
     * Android 13+: apply the per-app locale through the system LocaleManager so
     * every component (activity, service, notification, system UI) follows the
     * selected language. Call from Application/Activity onCreate.
     */
    fun applySystemLocale(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val tag = currentTag(context)
        if (tag == SYSTEM) return
        val manager = context.getSystemService(Context.LOCALE_SERVICE) as? LocaleManager ?: return
        manager.applicationLocales =
            LocaleList.forLanguageTags(currentLocale(tag).toLanguageTag())
    }

    /** Wrap [base] so its resources resolve in the selected app language. */
    fun apply(base: Context): Context {
        val tag = currentTag(base)
        val locale = currentLocale(tag)
        if (tag != SYSTEM) Locale.setDefault(locale)
        val config = Configuration(base.resources.configuration)
        config.setLocale(locale)
        config.setLayoutDirection(locale)
        return base.createConfigurationContext(config)
    }

    /** Force [res] to the selected app language (Android 12-). Call from getResources(). */
    fun localized(res: Resources, context: Context): Resources {
        val tag = currentTag(context)
        if (tag == SYSTEM) return res
        val locale = currentLocale(tag)
        val config = Configuration(res.configuration)
        config.setLocale(locale)
        config.setLayoutDirection(locale)
        res.updateConfiguration(config, res.displayMetrics)
        return res
    }
}
