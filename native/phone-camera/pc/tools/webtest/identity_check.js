// Static PHONE_CAMERA contract check for the Android app and the Safari page.
// Run from native/phone-camera: node pc/tools/webtest/identity_check.js
// This deliberately fails against the original MyCam identity, before the
// rebrand is applied. It is kept independent of Android SDK/JDK availability.
"use strict";

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..", "..", "..");
const read = (relative) => fs.readFileSync(path.join(root, relative), "utf8");
const files = {
  gradle: read("app/build.gradle.kts"),
  manifest: read("app/src/main/AndroidManifest.xml"),
  strings: read("app/src/main/res/values/strings.xml"),
  backup: read("app/src/main/res/xml/backup_rules.xml"),
  extraction: read("app/src/main/res/xml/data_extraction_rules.xml"),
  service: read("app/src/main/java/io/github/whoissaaif/mycam/WebcamService.kt"),
  nativeProtocol: read("app/src/main/java/io/github/whoissaaif/mycam/Protocol.kt"),
  wifiCrypto: read("app/src/main/java/io/github/whoissaaif/mycam/WifiCrypto.kt"),
  mainActivity: read("app/src/main/java/io/github/whoissaaif/mycam/MainActivity.kt"),
  qrPairing: read("app/src/main/java/io/github/whoissaaif/mycam/QrPairing.kt"),
  webScreen: read("app/src/main/java/io/github/whoissaaif/mycam/ui/WebcamScreen.kt"),
  settings: read("app/src/main/java/io/github/whoissaaif/mycam/ui/SettingsPage.kt"),
  instrumented: read("app/src/androidTest/java/io/github/whoissaaif/mycam/ExampleInstrumentedTest.kt"),
  webIndex: read("pc/companion/web/index.html"),
  webApp: read("pc/companion/web/app.js"),
  protocol: read("pc/companion/web/protocol.js"),
};

let failures = 0;
const check = (condition, message) => {
  if (condition) console.log("ok   " + message);
  else { failures++; console.log("FAIL " + message); }
};
const has = (text, pattern) => pattern instanceof RegExp ? pattern.test(text) : text.includes(pattern);
const notHas = (text, pattern) => !has(text, pattern);

// Android package identity: the Java/Kotlin namespace stays upstream-compatible,
// while the application id is intentionally a separate installable package.
check(has(files.gradle, 'namespace = "io.github.whoissaaif.mycam"'), "Android namespace remains io.github.whoissaaif.mycam");
check(has(files.gradle, 'applicationId = "ge.andrewaltair.phonecamera"'), "Android applicationId is ge.andrewaltair.phonecamera");
check(notHas(files.gradle, 'applicationId = "io.github.whoissaaif.mycam"'), "old MyCam applicationId is not used");
check(has(files.manifest, 'android:label="@string/app_name"'), "manifest label resolves through app_name");
check(has(files.strings, '<string name="app_name">PHONE_CAMERA</string>'), "Android visible app label is PHONE_CAMERA");
check(has(files.manifest, 'android:name=".MainActivity"') && has(files.manifest, 'android:name=".WebcamService"'), "manifest components resolve through the preserved namespace");
check(has(files.instrumented, 'assertEquals("ge.andrewaltair.phonecamera", appContext.packageName)'), "instrumented test expects the new install package");

// A separate package must not silently copy pairing secrets or promise migration.
check(has(files.backup, 'domain="sharedpref" path="mycam_pairing.xml"') && has(files.extraction, 'domain="sharedpref" path="mycam_pairing.xml"'), "pairing preferences remain excluded from backup/transfer");
check(has(files.service, 'const val PREFS = "mycam"') && has(files.qrPairing, 'const val PC_PORT = 47900'), "internal prefs and QR port stay wire-compatible");
check(has(files.mainActivity, 'const val ACCESSORY_MANUFACTURER = "MyCam"'), "AOA accessory manufacturer identity is preserved");
check(has(files.nativeProtocol, "const val VERSION = 3") && has(files.nativeProtocol, "const val PACKET_MAGIC = 0x4D43414D") && has(files.nativeProtocol, "const val WIRELESS_TCP_PORT = 47800"), "native MCAM and encrypted-Wi-Fi protocol constants are preserved");
check(has(files.wifiCrypto, '"MyCam pair v1"') && has(files.wifiCrypto, '"MyCam session v1"'), "native pairing-key derivation labels are preserved");

// Product links point at the real repository. The upstream project is credited,
// but is not presented as the PHONE_CAMERA product.
check(has(files.webIndex, "https://github.com/andrewaltair/PHONE_CAMERA"), "Safari page links to the PHONE_CAMERA repository");
check(has(files.webScreen, 'const val SOURCE_URL = "https://github.com/whoissaaif/mycam"'), "Android About links the MyCam source repository");
check(has(files.strings, /MyCam[\s\S]*MIT[\s\S]*whoissaaif/i), "Android About contains explicit MyCam MIT attribution");
check(has(files.webIndex, /MyCam[\s\S]*MIT[\s\S]*whoissaaif/i), "Safari page contains explicit MyCam MIT attribution");
for (const [name, text] of Object.entries(files)) {
  check(notHas(text, /https?:\/\/mycam\.dev/i), name + " has no mycam.dev product link");
}

// Safari identity and video-only/security boundaries.
check(has(files.webIndex, '<meta name="apple-mobile-web-app-title" content="PHONE_CAMERA">'), "Safari home-screen title is PHONE_CAMERA");
check(has(files.webIndex, /<title>PHONE_CAMERA<\/title>/) && has(files.webIndex, /class="title">PHONE_CAMERA<\/span>/), "Safari document/title bar identify PHONE_CAMERA");
check(has(files.webApp, "window.isSecureContext") && has(files.webApp, 'new WebSocket("wss://"'), "Safari page requires a secure context and encrypted WebSocket");
check(has(files.webApp, /getUserMedia\([\s\S]*audio:\s*false/) && notHas(files.webApp, /audio:\s*true/) && has(files.webApp, /VideoEncoder/), "Safari page captures video only and uses WebCodecs");
check(has(files.webApp, "while (q > 0 && !c.masks[q]) q--") && has(files.webApp, "masks.some((m) => m & P.FPS_BIT[60])") && has(files.webApp, "if (masks[2]) flags |= P.CAM.HAS_4K"), "Safari page reports and downgrades to camera-supported modes only");
check(has(files.settings, "val has4K = info != null && info.flags and Protocol.CAM_HAS_4K") && has(files.settings, "val mask = if (reported) info.fpsMask(settings.quality) else Protocol.FPS_30"), "Android settings gate quality and frame-rate choices on reported capabilities");
check(has(files.protocol, "const VERSION = 3") && has(files.protocol, "0x4D43414D") && has(files.protocol, "0x4D434D44"), "embedded iPhone protocol remains MCAM/MCMD v3");
check(notHas(files.webIndex, /virtual\s+microphone|virtual\s+mic/i) && notHas(files.webApp, /virtual\s+microphone|virtual\s+mic/i), "phone video path makes no virtual-microphone claim");

console.log(`${failures ? failures + " failed" : "all checks passed"}`);
process.exit(failures ? 1 : 0);
