import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

/**
 * 无感版本：手动名来自仓库根目录 version.name，构建号按本地日期 YYYYMMDD。
 * Android Studio / flutter run 走 Gradle 时自动生效，系统「应用信息」显示完整 versionName。
 */
fun resolveRollingVersion(): Pair<String, Int> {
    val nameFile = File(project.projectDir, "../../version.name")
    val name = nameFile
        .takeIf { it.isFile }
        ?.readText()
        ?.trim()
        ?.takeIf { it.isNotEmpty() }
        ?: "1.0.0"
    val build = SimpleDateFormat("yyyyMMdd", Locale.US).format(Date())
    val code = build.toInt()
    return "$name+$build" to code
}

val rollingVersion = resolveRollingVersion()

android {
    namespace = "com.sifli.sifli_companion"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "top.jinsc.helm_one"
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        // 覆盖 pubspec / Flutter 插件注入值，保证每次 Run 都带当日构建号
        versionName = rollingVersion.first
        versionCode = rollingVersion.second
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}

println("Helm One → versionName=${rollingVersion.first} versionCode=${rollingVersion.second}")
