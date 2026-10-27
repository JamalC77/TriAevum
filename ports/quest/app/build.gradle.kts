plugins { id("com.android.application") }

dependencies { testImplementation("junit:junit:4.13.2") }

// Native compilation is a separate CMake build (tools/quest/build_quest.sh), never a Gradle side effect.
// The title module is never packaged: it is side-loaded into app-private storage (TitleInstaller).
val sdlSource = providers.gradleProperty("triaevumSdlSource")
val nativeStage = providers.gradleProperty("triaevumNativeStage")
val questAssets = providers.gradleProperty("triaevumQuestAssets")
// Immersive mode (TriAevumXrActivity, the OpenXR loader); false only for a QUEST_OPENXR=0 build.
val questOpenXr = providers.gradleProperty("triaevumQuestOpenXr").map { it.toBoolean() }.orElse(true)
val versionCodeValue = (System.getenv("TRIAEVUM_QUEST_VERSION_CODE") ?: "1").toInt()
val versionNameValue = System.getenv("TRIAEVUM_QUEST_VERSION_NAME") ?: "0.8.0-beta.1.2-quest.dev"
val keystore = System.getenv("TRIAEVUM_QUEST_KEYSTORE_FILE")?.let { file(it) }?.takeIf { it.isFile }

android {
    namespace = "org.triaevum.quest"
    compileSdk = 35
    defaultConfig {
        applicationId = "org.triaevum.quest"
        minSdk = 32
        targetSdk = 34
        versionCode = versionCodeValue
        versionName = versionNameValue
        ndk { abiFilters += "arm64-v8a" }
        buildConfigField("boolean", "QUEST_OPENXR", questOpenXr.get().toString())
    }
    buildFeatures { buildConfig = true }
    signingConfigs {
        if (keystore != null) {
            create("quest") {
                storeFile = keystore
                storeType = "PKCS12"
                storePassword = System.getenv("TRIAEVUM_QUEST_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("TRIAEVUM_QUEST_KEY_ALIAS")
                keyPassword = System.getenv("TRIAEVUM_QUEST_KEY_PASSWORD")
            }
        }
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            // Development builds: run-as must work for logs, data staging and the title module.
            isDebuggable = true
            isJniDebuggable = true
            signingConfig = if (keystore != null) signingConfigs.getByName("quest") else signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        if (sdlSource.isPresent) java.srcDir("${sdlSource.get()}/android-project/app/src/main/java")
        if (nativeStage.isPresent) jniLibs.srcDir(nativeStage.get())
        if (questAssets.isPresent) assets.srcDir(questAssets.get())
    }
    packaging {
        jniLibs.useLegacyPackaging = true
        // Native libraries are stripped by the staging step; keep symbols out of Gradle's hands.
        jniLibs.keepDebugSymbols += "**/*.so"
    }
    lint { checkReleaseBuilds = false }
}

tasks.register("checkNativeStage") {
    doLast {
        check(sdlSource.isPresent && nativeStage.isPresent && questAssets.isPresent) {
            "Supply -PtriaevumSdlSource, -PtriaevumNativeStage and -PtriaevumQuestAssets from tools/quest/build_quest.sh."
        }
        val required = listOf("SDL2", "TriAevum", "triaevum_title_bootstrap", "c++_shared") +
            (if (questOpenXr.get()) listOf("openxr_loader") else emptyList())
        for (name in required) {
            check(file("${nativeStage.get()}/arm64-v8a/lib$name.so").isFile) { "Missing native library: $name" }
        }
        check(!file("${nativeStage.get()}/arm64-v8a/libtriaevum_title_aot.so").exists()) {
            "The title module must never be packaged into the APK"
        }
        check(file("${questAssets.get()}/title-abi.json").isFile) { "Missing assets/title-abi.json" }
    }
}
tasks.named("preBuild") { dependsOn("checkNativeStage") }
