plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.ksp)
    alias(libs.plugins.hilt)
}

import java.util.Properties

val localProps = Properties().apply {
    rootProject.file("local.properties").takeIf { it.exists() }?.inputStream()?.use(::load)
}

// Prebuilt FFmpeg/OpenSSL are cross-compiled per ABI. The universal build keeps
// 32-bit + 64-bit ARM only to shrink the APK (x86_64 is only useful for emulators),
// but the per-ABI split build re-adds x86_64 so emulator DLNA/cast testing can run
// a native x86_64 APK instead of relying on arm-on-x86 translation.
// Switch the universal list back to the full set if it should also cover emulators.
val allAbis = listOf("arm64-v8a", "armeabi-v7a")

// Opt-in per-ABI APK splits: build with -PsplitApks=true to emit
// app-arm64-v8a-release.apk + app-armeabi-v7a-release.apk instead of one
// universal APK. AGP forbids ndk abiFilters and splits.abi at the same
// time, so the split build drops the filters and lets the splits
// include-list drive the ABI set (arm64-v8a + armeabi-v7a + x86_64 for
// emulator testing).
// Default (property absent): unchanged universal-APK build.
// Note: the sanitize buildType (HWASan, arm64-only) is not compatible with
// split mode; run assembleSanitize without the property.
val splitApksByAbi = (project.findProperty("splitApks") as String?)?.toBoolean() ?: false

android {
    namespace = "io.github.jqssun.airplay"
    compileSdk = 37
    ndkVersion = "27.0.12077973"

    if (localProps.containsKey("storeFile")) {
        signingConfigs {
            create("release") {
                storeFile = rootProject.file(localProps.getProperty("storeFile"))
                // passwords may come from the environment (CI / shared machines) with
                // local.properties as the fallback; that file is gitignored either way
                storePassword = System.getenv("FLOWCAST_KEYSTORE_PASSWORD")
                    ?: localProps.getProperty("storePassword")
                keyAlias = localProps.getProperty("keyAlias")
                keyPassword = System.getenv("FLOWCAST_KEY_PASSWORD")
                    ?: localProps.getProperty("keyPassword")
            }
        }
    }

    defaultConfig {
        applicationId = "io.github.zwkit.flowcast"
        minSdk = 24
        targetSdk = 37
        versionCode = 214
        versionName = "2.1.4"

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildTypes {
        debug {
            if (!splitApksByAbi) ndk { abiFilters += allAbis }
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfigs.findByName("release")?.let { signingConfig = it }
            if (!splitApksByAbi) ndk { abiFilters += allAbis }
        }
        // debuggable build with HWASan (arm64) + UBSan in native code
        create("sanitize") {
            initWith(getByName("debug"))
            matchingFallbacks += "debug"
            if (!splitApksByAbi) ndk {
                abiFilters.clear()
                abiFilters += "arm64-v8a"
            }
            externalNativeBuild {
                cmake { arguments += "-DSANITIZE=ON" }
            }
        }
    }

    // Per-ABI APK splits (opt-in via -PsplitApks=true, see val splitApksByAbi above).
    // Each split APK is about half the universal one: native libs dominate.
    // Set isUniversalApk = true to also emit a combined app-release.apk.
    if (splitApksByAbi) {
        splits {
            abi {
                isEnable = true
                reset()
                include("arm64-v8a", "armeabi-v7a", "x86_64")
                isUniversalApk = false
            }
        }
    }

    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        prefab = true
    }
}

// APK 输出文件名：flowcast-<版本号>-release.apk；启用 -PsplitApks=true 分包时为
// flowcast-<版本号>-<abi>-release.apk（arm64-v8a / armeabi-v7a / x86_64）。
// AGP 9 起 archivesBaseName / applicationVariants 均已移除，按官方推荐改用
// androidComponents 重命名；默认名形如 app-<abi>-release.apk，从中提取 ABI 作后缀。
// 注意：新增 ABI 时必须同步更新下面的正则，否则该分包会落入通用包命名分支。
val apkVersionName = android.defaultConfig.versionName

androidComponents {
    onVariants(selector().withBuildType("release")) { variant ->
        variant.outputs.forEach { output ->
            val current = output.outputFileName.get()
            val abi = Regex("app-(arm64-v8a|armeabi-v7a|x86_64)-release\\.apk")
                .find(current)?.groupValues?.get(1)
            output.outputFileName.set(
                if (abi != null) "flowcast-$apkVersionName-$abi-release.apk"
                else "flowcast-$apkVersionName-release.apk"
            )
        }
    }
}

// The UxPlay patches in src/main/cpp/patches/UxPlay are already applied to
// third_party/UxPlay at setup time, so no git-based patch task is required.
// (Original upstream applied them via a git-dependent Gradle task; that task
//  has been removed so the project can build without git installed.)

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.lifecycle.viewmodel)
    implementation(libs.androidx.datastore.prefs)
    implementation(libs.androidx.media)
    implementation(libs.media3.exoplayer)
    implementation(libs.media3.exoplayer.hls)
    implementation(libs.media3.ui.compose.material3)
    implementation(libs.media3.transformer)
    implementation(libs.kotlinx.coroutines)
    implementation(libs.oboe)
    // vendored jaronly: the build runs --offline, so QR generation ships as a local file
    // dependency instead of being resolved from Maven Central
    implementation(files("libs/zxing-core-3.5.3.jar"))
    // vendored jaronly for the same reason: RtspMediaSource + the RTP readers that make
    // rtsp:// playable. classes.jar of androidx.media3:media3-exoplayer-rtsp:1.11.1 — that
    // AAR carries no resources, no manifest entries and no consumer proguard rules, so the
    // jar alone is an exact equivalent. Must stay in lockstep with libs.versions.toml media3.
    implementation(files("libs/media3-exoplayer-rtsp-1.11.1.jar"))

    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons)
    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)
    implementation(libs.hilt.navigation.compose)

    testImplementation(libs.junit)
}
