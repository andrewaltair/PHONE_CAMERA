package io.github.whoissaaif.mycam

import android.graphics.SurfaceTexture
import android.os.Build

import android.view.Surface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/** Why the phone's own preview is or isn't running (docs/redesign-v2.md 6.1). */
enum class PreviewSupport {
    /** Nothing has been tried yet (no camera running). */
    Unknown,

    /** The preview is a second output of the running capture session. */
    Ok,

    /** Android 8 or older: a second output can't be added to a session that is already running. */
    NeedsAndroid9,

    /** This frame rate uses a constrained high-speed session, which takes no extra output here. */
    NotAtThisFrameRate,

    /** The camera refused the second output. The stream is never sacrificed for the preview. */
    Refused,

    /** The phone is too hot: the preview turned itself off. */
    TooHot,
}

/**
 * The phone's live preview (docs/redesign-v2.md 6.1), shared between the Camera screen, the activity and the
 * service.
 *
 * The screen hands over the Surface of its TextureView; the service passes it to [CameraStreamer] as a
 * **second output of the running capture session**, so what the PC receives never changes and the camera is
 * never restarted. The preview runs only when all of these hold:
 *  - the user's Preview toggle is on (persisted, default on);
 *  - the screen is showing it (the app is in the foreground and the Dim screen is not up);
 *  - the phone is not in a severe thermal state.
 */
object PhonePreview {
    /** The user's toggle (persisted in [WebcamService.PREF_PREVIEW]). */
    private val _on = MutableStateFlow(true)
    val on: StateFlow<Boolean> = _on.asStateFlow()

    private val _support = MutableStateFlow(PreviewSupport.Unknown)
    val support: StateFlow<PreviewSupport> = _support.asStateFlow()

    /** True while frames are being delivered to the preview (the screen crossfades on this). */
    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running.asStateFlow()

    private var surface: Surface? = null
    private var appVisible = false // The app is in the foreground and the Dim screen is not up.
    private var onPage = false     // The Camera screen (the only screen with a preview) is the one showing.
    private var tooHot = false

    /** Set by the service: it owns the camera thread and the streamer. */
    @Volatile
    var onTargetChanged: ((Surface?) -> Unit)? = null

    /** Android 8 and older never get a preview: a running session can't take another output. */
    val possible: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P

    fun setOn(on: Boolean) {
        if (_on.value == on) return
        _on.value = on
        if (!on) _running.value = false
        publish()
    }

    /** The app is in the foreground and the Dim screen is not up (the activity decides this). */
    fun setAppVisible(visible: Boolean) {
        if (appVisible == visible) return
        appVisible = visible
        if (!visible) _running.value = false
        publish()
    }

    /** The Camera screen — the only screen that shows a preview — is the one on display. */
    fun setPreviewPage(on: Boolean) {
        if (onPage == on) return
        onPage = on
        if (!on) _running.value = false
        publish()
    }

    /** The TextureView's surface arrived (or went away). */
    fun setSurface(surface: Surface?) {
        if (this.surface == surface) return
        this.surface = surface
        if (surface == null) _running.value = false
        publish()
    }

    /** PowerManager.THERMAL_STATUS_*: at SEVERE and above the preview turns itself off and says why. */
    fun setThermal(status: Int) {
        val hot = status >= THERMAL_SEVERE
        if (hot == tooHot) return
        tooHot = hot
        if (hot) {
            _running.value = false
            _support.value = PreviewSupport.TooHot
        } else if (_support.value == PreviewSupport.TooHot) {
            _support.value = PreviewSupport.Unknown
        }
        publish()
    }

    /** Reported by the streamer once it knows whether this mode can carry a preview. */
    fun reportSupport(support: PreviewSupport) {
        if (tooHot && support != PreviewSupport.Ok) return
        _support.update { if (tooHot) PreviewSupport.TooHot else support }
        _running.value = support == PreviewSupport.Ok && target() != null
    }

    /** The camera stopped: nothing is being previewed, and what the next mode can do is unknown again. */
    fun cameraStopped() {
        _running.value = false
        if (_support.value != PreviewSupport.TooHot) _support.value = PreviewSupport.Unknown
    }

    /** The surface the camera should use right now, or null when the preview must not run. */
    fun target(): Surface? = if (_on.value && appVisible && onPage && !tooHot && possible) surface else null

    private fun publish() {
        if (!possible && _on.value) _support.value = PreviewSupport.NeedsAndroid9
        onTargetChanged?.invoke(target())
    }

    /** Used by the UI to size the preview; a default buffer size keeps the aspect right before the first frame. */
    fun prepare(texture: SurfaceTexture, width: Int, height: Int) {
        if (width > 0 && height > 0) texture.setDefaultBufferSize(width, height)
    }

    /** PowerManager.THERMAL_STATUS_SEVERE (API 29); spelled out so API 24 builds don't need the field. */
    const val THERMAL_SEVERE = 3
}
