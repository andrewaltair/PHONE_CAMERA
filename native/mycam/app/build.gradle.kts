import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Release signing: keystore.properties (git-ignored, made by tools/create-release-key.ps1) points at the
// release keystore kept outside the repo. Without it, release builds come out unsigned.
val keystoreProps = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "io.github.whoissaaif.mycam"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "io.github.whoissaaif.mycam"
        minSdk = 24
        targetSdk = 37
        versionCode = 15
        versionName = "1.5.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        if (keystoreProps.getProperty("storeFile") != null) {
            create("release") {
                storeFile = file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("release")
            optimization {
                enable = true
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
    // Robolectric (the opt-in phone UI snapshots) needs the merged resources.
    testOptions.unitTests.isIncludeAndroidResources = true
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    // QR pairing (IMPROVEMENTS.md 12): the scanner decodes with ZXing, bundled, so no Google Play services needed.
    implementation(libs.zxing.core)
    testImplementation(libs.junit)
    testImplementation(platform(libs.androidx.compose.bom))
    testImplementation(libs.androidx.compose.ui.test.junit4)
    testImplementation(libs.robolectric)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
// ProtocolTest reads the shared golden vectors; re-run unit tests whenever they change.
val protocolGolden = rootProject.file("protocol/golden.txt")
tasks.withType<Test>().configureEach {
    inputs.file(protocolGolden).withPathSensitivity(PathSensitivity.RELATIVE)
}

// Phone UI snapshots (src/test/.../snapshots) render the screen states to PNGs with Robolectric's native
// graphics. They're opt-in and record-only (nothing is compared), so a normal test run skips them:
//   gradlew testDebugUnitTest -Psnapshots --tests "*PhoneSnapshots*"
// The PNGs land in app/build/snapshots.
val snapshots = providers.gradleProperty("snapshots").isPresent
val snapshotDir = layout.buildDirectory.dir("snapshots").get().asFile.absolutePath
tasks.withType<Test>().configureEach {
    if (snapshots) {
        systemProperty("mycam.snapshots", snapshotDir)
        outputs.upToDateWhen { false }
    } else {
        exclude("**/snapshots/**")
    }
}
