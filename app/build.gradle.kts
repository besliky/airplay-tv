import java.security.MessageDigest
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

// Version is taken from -PversionName (set by CI from the git tag) or defaults to the
// in-tree development version. The version code is derived from it so that every
// release sorts correctly: 1.2.3 -> 10203.
val appVersionName: String = (findProperty("versionName") as String?)?.removePrefix("v") ?: "0.2.0"
val appVersionCode: Int = appVersionName.split('.', '-').take(3)
    .map { it.toIntOrNull() ?: 0 }
    .let { (it + listOf(0, 0, 0)).take(3) }
    .let { (major, minor, patch) -> major * 10000 + minor * 100 + patch }

// GitHub repository whose releases the in-app update installs (-PupdateRepository=owner/name for a fork).
val updateRepository: String = (findProperty("updateRepository") as String?) ?: "besliky/airplay-tv"

// Release signing. Credentials come from environment variables (CI secrets) or from an
// untracked keystore.properties file. When neither is present, release builds are signed
// with the local debug key so that they stay installable for testing.
val releaseSigning: Map<String, String>? = run {
    val env = System.getenv()
    val fromEnv = listOf("RELEASE_KEYSTORE_FILE", "RELEASE_KEYSTORE_PASSWORD", "RELEASE_KEY_ALIAS", "RELEASE_KEY_PASSWORD")
        .associateWith { env[it].orEmpty() }
    if (fromEnv.values.all { it.isNotEmpty() } && file(fromEnv.getValue("RELEASE_KEYSTORE_FILE")).exists()) {
        return@run fromEnv
    }
    val propsFile = rootProject.file("keystore.properties")
    if (!propsFile.exists()) return@run null
    val props = Properties().apply { propsFile.inputStream().use { load(it) } }
    mapOf(
        "RELEASE_KEYSTORE_FILE" to rootProject.file(props.getProperty("storeFile", "")).path,
        "RELEASE_KEYSTORE_PASSWORD" to props.getProperty("storePassword", ""),
        "RELEASE_KEY_ALIAS" to props.getProperty("keyAlias", ""),
        "RELEASE_KEY_PASSWORD" to props.getProperty("keyPassword", ""),
    ).takeIf { m -> m.values.all { it.isNotEmpty() } && file(m.getValue("RELEASE_KEYSTORE_FILE")).exists() }
}

android {
    namespace = "io.github.besliky.airplaytv"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "io.github.besliky.airplaytv"
        minSdk = 26
        targetSdk = 35
        versionCode = appVersionCode
        versionName = appVersionName
        buildConfigField("String", "UPDATE_REPOSITORY", "\"$updateRepository\"")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none", "-DAIRPLAYTV_ANDROID=ON")
            }
        }
    }

    signingConfigs {
        create("release") {
            val s = releaseSigning
            if (s != null) {
                storeFile = file(s.getValue("RELEASE_KEYSTORE_FILE"))
                storePassword = s.getValue("RELEASE_KEYSTORE_PASSWORD")
                keyAlias = s.getValue("RELEASE_KEY_ALIAS")
                keyPassword = s.getValue("RELEASE_KEY_PASSWORD")
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = if (releaseSigning != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug {
            isJniDebuggable = true
        }
    }

    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a", "x86_64")
            isUniversalApk = true
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../core/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            // Compressed native libraries keep the download small; they are extracted once at install.
            useLegacyPackaging = true
        }
        resources {
            excludes += listOf("/META-INF/*.version", "/META-INF/**/*.kotlin_module", "kotlin/**", "DebugProbesKt.bin")
        }
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = true
        warningsAsErrors = false
        disable += listOf("GradleDependency", "NewerVersionAvailable", "AndroidGradlePluginVersion", "OldTargetApi")
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.ext.junit)
}

// The in-app "Open Source Licenses" page shows these files; they are packaged as an asset.
abstract class LicenseAssetsTask : DefaultTask() {
    @get:InputFiles
    abstract val sources: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun generate() {
        val separator = "\n\n" + "=".repeat(72) + "\n\n"
        val text = sources.files.joinToString(separator) { it.readText().trimEnd() } + "\n"
        val out = outputDir.get().asFile
        out.mkdirs()
        File(out, "licenses.txt").writeText(text)
    }
}

val licenseAssets = tasks.register<LicenseAssetsTask>("generateLicenseAssets") {
    sources.from(
        rootProject.file("NOTICE"),
        rootProject.file("THIRD_PARTY_LICENSES.md"),
        rootProject.file("LICENSE"),
        rootProject.file("core/third_party/mbedtls/LICENSE"),
        rootProject.file("core/third_party/monocypher/LICENCE.md"),
    )
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(licenseAssets, LicenseAssetsTask::outputDir)
    }
}

// Collects the release APKs under user-facing names (AirPlayTV-v1.2.3-arm64.apk, ...) together
// with SHA-256 checksums. CI publishes the content of build/dist as the release assets.
val abiLabels = mapOf(
    "arm64-v8a" to "arm64",
    "armeabi-v7a" to "armv7",
    "x86_64" to "x86_64",
    "universal" to "universal",
)

tasks.register("packageReleaseApks") {
    group = "distribution"
    description = "Copies release APKs to build/dist with release names and writes SHA256SUMS."
    dependsOn("assembleRelease")
    val apkDir = layout.buildDirectory.dir("outputs/apk/release")
    val distDir = layout.buildDirectory.dir("dist")
    val versionLabel = appVersionName
    inputs.dir(apkDir)
    outputs.dir(distDir)
    doLast {
        val out = distDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        val sums = StringBuilder()
        apkDir.get().asFile.listFiles { f -> f.extension == "apk" }.orEmpty().sortedBy { it.name }.forEach { apk ->
            val abi = abiLabels.entries.firstOrNull { apk.name.contains("-${it.key}-") }?.value ?: return@forEach
            val target = File(out, "AirPlayTV-v$versionLabel-$abi.apk")
            apk.copyTo(target, overwrite = true)
            val digest = MessageDigest.getInstance("SHA-256").digest(target.readBytes())
            sums.append(digest.joinToString("") { "%02x".format(it) }).append("  ").append(target.name).append('\n')
        }
        File(out, "SHA256SUMS").writeText(sums.toString())
        logger.lifecycle("Release APKs written to $out")
    }
}
