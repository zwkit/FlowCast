package io.github.jqssun.airplay

import android.app.Application
import android.content.Context
import dagger.hilt.android.HiltAndroidApp

@HiltAndroidApp
class AirPlayApp : Application() {
    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(Locales.apply(base))
    }

    override fun onCreate() {
        super.onCreate()
        Locales.applySystemLocale(this)
    }

    override fun getResources(): android.content.res.Resources =
        Locales.localized(super.getResources(), this)
}
