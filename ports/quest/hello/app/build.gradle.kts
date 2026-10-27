plugins { id("com.android.application") }

// Native libraries come prebuilt from ../build.sh (CMake + Ninja), never from
// a Gradle-driven native build, as in ports/android.
val sdlSource = providers.gradleProperty("helloSdlSource")
val nativeStage = providers.gradleProperty("helloNativeStage")
// CI decodes TRIAEVUM_QUEST_KEYSTORE_BASE64 into this file (PKCS12).
val keystore = providers.environmentVariable("TRIAEVUM_QUEST_KEYSTORE_FILE")

android {
    namespace = "org.triaevum.quest.hello"
    compileSdk = 35
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "org.triaevum.quest.hello"
        minSdk = 32
        targetSdk = 34
        versionCode = (System.getenv("GITHUB_RUN_NUMBER") ?: "1").toInt()
        versionName = "0.1.0-hello"
        ndk { abiFilters += "arm64-v8a" }
    }
    signingConfigs {
        create("release") {
            if (keystore.isPresent) {
                storeFile = file(keystore.get())
                storeType = "pkcs12"
                storePassword = System.getenv("TRIAEVUM_QUEST_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("TRIAEVUM_QUEST_KEY_ALIAS") ?: "triaevum-quest"
                keyPassword = System.getenv("TRIAEVUM_QUEST_KEY_PASSWORD")
            }
        }
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (keystore.isPresent) "release" else "debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        if (sdlSource.isPresent) java.srcDir("${sdlSource.get()}/android-project/app/src/main/java")
        if (nativeStage.isPresent) jniLibs.srcDir(nativeStage.get())
    }
    packaging { jniLibs.useLegacyPackaging = true }
    // A sideloaded test app: Play's target-SDK and other release lint gates do not apply.
    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }
}

tasks.register("checkNativeStage") {
    doLast {
        check(sdlSource.isPresent && nativeStage.isPresent) {
            "Supply -PhelloSdlSource and -PhelloNativeStage (ports/quest/hello/build.sh does)."
        }
        for (name in listOf("triaevum_hello", "SDL2", "openxr_loader", "c++_shared")) {
            check(file("${nativeStage.get()}/arm64-v8a/lib$name.so").isFile) { "Missing native library: $name" }
        }
    }
}
tasks.named("preBuild") { dependsOn("checkNativeStage") }
