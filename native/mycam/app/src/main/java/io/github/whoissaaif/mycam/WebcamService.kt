package io.github.whoissaaif.mycam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.OrientationEventListener
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.FileInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Foreground service that owns the USB accessory link to the PC and the camera.
 * The PC decides when to stream (START/STOP) so the camera only runs while a PC app is using the webcam.
 */
class WebcamService : Service() {

    data class UiState(
        val connected: Boolean = false,
        val streaming: Boolean = false,
        val paused: Boolean = false,
        /** Video and camera-control settings (persisted). */
        val camera: CameraStreamer.Settings = CameraStreamer.Settings(),
        /** What the active camera supports; null until the camera has been opened once. */
        val cameraInfo: Protocol.CameraInfo? = null,
        val facing: Int = Protocol.FACING_BACK,
        val resolution: String = "",
        val error: String? = null,
        /** PowerManager.THERMAL_STATUS_* (0 = none); the UI warns from MODERATE up. */
        val thermal: Int = 0,
        /** Wireless mode is on (the phone can be found and connected to over Wi-Fi). */
        val wirelessOn: Boolean = false,
        /** The current link is Wi-Fi (else USB). */
        val wireless: Boolean = false,
        /** This phone's address on Wi-Fi, shown so the user can tell which network it is on. */
        val wirelessAddress: String? = null,
        /** A PC that is pairing over Wi-Fi and waiting for the user to allow it, and the code both screens show. */
        val pendingPc: String? = null,
        val pendingCode: String? = null,
        /** SystemClock.elapsedRealtime() at which the pending pairing times out (the dialog counts down to it). */
        val pendingDeadline: Long = 0L,
        /** Name of a PC whose pairing request timed out unanswered; the dialog says so until dismissed. */
        val pairingTimedOut: String? = null,
        /** The PCs this phone is paired with (they connect without asking). */
        val pairedPcs: List<PairedPc> = emptyList(),
        /** Name of the PC connected over Wi-Fi. */
        val wirelessPc: String? = null,
        /** IP address of the PC connected over Wi-Fi (marks it in the nearby list). */
        val wirelessPcIp: String? = null,
        /** PCs looking for phones on this Wi-Fi right now (probes in the last 6 s), by name. */
        val nearbyPcs: List<NearbyPc> = emptyList(),
        /** IP of a nearby PC this phone has asked to connect ("connect to me"), until it does or gives up. */
        val invitingPc: String? = null,
        /** "Scan for PCs" is running (6 s: three of the PC's probe intervals). */
        val scanning: Boolean = false,
        /** A scan has finished since wireless mode was turned on (the UI can say "No PC found"). */
        val scanned: Boolean = false,
        /** QR pairing after a scan (PROTOCOL.md "QR pairing"); null when nothing is going on. */
        val qr: QrStatus? = null,
        /** The PC a new pairing (QR code or 6-digit code) just connected to: "Paired with <name>" until OK. */
        val justPaired: String? = null,
        /**
         * The connected PC's name while it is waiting to be allowed to use the camera. A QR pairing has no
         * moment where the user agrees to anything, and a PC paired before this existed never had one, so the
         * phone connects, keeps the camera off and asks once. Null when nothing is waiting.
         */
        val cameraGate: String? = null,
    )

    /**
     * "Connecting to [pcName]…" while [failure] is null (the name arrives once paired), else why it didn't
     * work. [trying] means the phone is still at it and [failure] is only the reason heard so far: a refusal
     * arrives as an unauthenticated datagram, so it is shown but not obeyed until the attempt runs out.
     */
    data class QrStatus(
        val pcName: String? = null,
        val failure: QrPairing.Failure? = null,
        val trying: Boolean = false,
    )

    private lateinit var cameraThread: HandlerThread
    private lateinit var camera: Handler
    private var streamer: CameraStreamer? = null
    private var orientationListener: OrientationEventListener? = null
    private var thermalListener: Any? = null // PowerManager.OnThermalStatusChangedListener (API 29+).
    private var wakeLock: PowerManager.WakeLock? = null

    // The link to the PC: a USB accessory or (wireless mode) a TCP socket. Opened and closed on the main thread.
    private var accessory: UsbAccessory? = null
    private var pfd: ParcelFileDescriptor? = null
    private var socket: Socket? = null
    @Volatile private var output: OutputStream? = null
    @Volatile private var linked = false
    @Volatile private var linkId = 0 // Bumped per link, so a late "read ended" from an old link is ignored.
    private var readerThread: Thread? = null
    private var foreground = false

    // Wireless mode (main thread).
    private val main by lazy { Handler(mainLooper) }
    private var wireless: WirelessServer? = null
    private var wirelessOn = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var handshaking = false                      // A PC is pairing or proving its pairing key.
    private var pendingAnswer: ((Boolean) -> Unit)? = null // Waiting for Allow / Don't allow on a pairing.
    private val pairedPcs by lazy { PairedPcs.open(this) }
    private val nearby = NearbyPcs()
    /** Drops PCs that stopped probing; runs every second while the nearby list isn't empty. */
    private val expireNearby = object : Runnable {
        override fun run() {
            publishNearby()
            if (!nearby.isEmpty()) main.postDelayed(this, 1_000)
        }
    }
    private val endScan = Runnable { _state.update { it.copy(scanning = false, scanned = true) } }

    /**
     * QR pairing in progress (PROTOCOL.md "QR pairing"): the scanned code, until when to keep trying, and
     * whether that PC's probe has been heard. Set and cleared on the main thread; the handshake thread reads
     * the code (it is immutable).
     */
    private class QrPending(val code: QrPairing.Code, val until: Long) {
        var heard = false
        var pcName: String? = null
        var invitedAt = 0L                            // When the last invite went out, for [refused].
        var refused: QrPairing.Failure? = null        // The reason the PC last gave, if any.
    }
    @Volatile private var qrPending: QrPending? = null

    /** Every 2 s while a QR pairing waits: invite the PC once its probe has been heard, or give up in time. */
    private val qrTick = object : Runnable {
        override fun run() {
            val p = qrPending ?: return
            val now = SystemClock.elapsedRealtime()
            if (linked) { finishQr(null); return }
            if (now > p.until) {
                finishQr(p.refused ?: if (p.heard) QrPairing.Failure.NoSession else QrPairing.Failure.NoPc)
                return
            }
            val pc = nearby.find(p.code.host, now)
            if (pc != null && pc.port > 0 && !handshaking) {
                if (!p.heard || p.pcName != pc.name) {
                    p.heard = true
                    p.pcName = pc.name
                    _state.update { it.copy(qr = QrStatus(pc.name, p.refused, trying = true)) }
                }
                p.invitedAt = now
                wireless?.qrInvite(pc.ip, pc.port, p.code.code)
            }
            main.postDelayed(this, QR_RESEND_MS)
        }
    }

    /**
     * "Connect to me" asked of one nearby PC: its IP, the probes left to send and the resend timer. One
     * datagram may be lost, so it goes out every 2 s for INVITE_TRIES tries, and stops as soon as that PC
     * connects (or wireless mode goes off).
     */
    private var invitingIp: String? = null
    private var invitesLeft = 0
    private val resendInvite = object : Runnable {
        override fun run() {
            val ip = invitingIp ?: return
            if (!wirelessOn || linked || handshaking) { stopInviting(); return }
            val pc = nearby.find(ip, SystemClock.elapsedRealtime())
            if (pc == null || pc.port == 0 || invitesLeft <= 0) {
                Log.i(TAG, "no answer from $ip to \"connect to me\"")
                stopInviting()
                return
            }
            --invitesLeft
            wireless?.askToConnect(pc.ip, pc.port)
            main.postDelayed(this, INVITE_EVERY_MS)
        }
    }

    // Owned by the camera thread.
    private var wantStreaming = false
    // The connected PC hasn't been allowed to use the camera yet: stay connected, keep the camera off and
    // wait for the user. Set when a link opens or closes (main thread) and read while handling the PC's
    // commands (camera thread), so both are @Volatile.
    @Volatile private var cameraBlocked = false
    @Volatile private var gatedPcId: ByteArray? = null
    private var paused = false // User pause (phone or PC). Persisted so a reconnect never turns the camera back on.
    private var camSettings = CameraStreamer.Settings()
    private var lastCameraInfo: Protocol.CameraInfo? = null
    private var facing = Protocol.FACING_BACK
    private var lastConfig: ByteArray? = null
    private var deviceRotation = 0
    private var framesSent = 0
    private var bytesSent = 0L
    private var lastStatsLog = 0L

    // Adaptive streaming: keep the bitrate within what the USB link can carry (camera thread only).
    private var linkBitrate = 0           // Current encoder bitrate.
    private var linkCeiling = Int.MAX_VALUE // Don't climb back above 90% of a bitrate the link fell behind at.
    private var linkCeilingSince = 0L
    private var dropUntilKey = false
    private var framesDropped = 0
    private var lastBitrateChange = 0L
    private var slowestWriteMs = 0L       // Slowest frame write since the last bitrate change.

    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val detached = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
            if (accessory != null && (detached == null || detached == accessory)) {
                Log.i(TAG, "Accessory detached")
                val id = linkId
                camera.post { onLinkLost(id) }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        cameraThread = HandlerThread("camera").apply { start() }
        camera = Handler(cameraThread.looper)
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        facing = prefs.getInt(PREF_FACING, Protocol.FACING_BACK)
        paused = prefs.getBoolean(PREF_PAUSED, false)
        camSettings = loadCameraSettings(prefs)
        _state.update { UiState(facing = facing, paused = paused, camera = camSettings, pairedPcs = pairedPcs.list()) }
        // The phone's own preview (docs/redesign-v2.md 6.1): the screen hands over a surface, this thread hands it
        // to the running capture session. The streamer decides whether the mode can carry it.
        PhonePreview.setOn(prefs.getBoolean(PREF_PREVIEW, true))
        PhonePreview.onTargetChanged = { surface -> camera.post { streamer?.setPreviewSurface(surface) } }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val listener = PowerManager.OnThermalStatusChangedListener { status ->
                _state.update { it.copy(thermal = status) }
                // Severe heat turns the preview off by itself; the stream is never sacrificed for it.
                PhonePreview.setThermal(status)
                camera.post { remoteLog("thermal status $status") }
            }
            getSystemService(PowerManager::class.java).addThermalStatusListener(mainExecutor, listener)
            thermalListener = listener
        }
        ContextCompat.registerReceiver(
            this, detachReceiver, IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN) return
                val snapped = ((orientation + 45) / 90 % 4) * 90
                // Hysteresis: only switch when clearly past the 45 degree boundary.
                val diff = Math.abs(((orientation - deviceRotation + 540) % 360) - 180)
                if (snapped != deviceRotation && diff > 60) {
                    camera.post {
                        deviceRotation = snapped
                        sendOrientation()
                    }
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_SET_FACING -> {
                val f = intent.getIntExtra(EXTRA_FACING, Protocol.FACING_BACK)
                camera.post { setFacing(f) }
                return START_NOT_STICKY
            }
            ACTION_COMMAND -> {
                // The phone UI uses the same commands as the PC (quality, fps, zoom, exposure, torch, focus).
                val cmd = intent.getIntExtra(EXTRA_CMD, 0)
                val arg = intent.getIntExtra(EXTRA_ARG, 0)
                camera.post { handleCommand(cmd, arg) }
                return START_NOT_STICKY
            }
            ACTION_SET_PAUSED -> {
                val p = intent.getBooleanExtra(EXTRA_PAUSED, false)
                camera.post { setPaused(p) }
                return START_NOT_STICKY
            }
            ACTION_WIRELESS -> {
                setWireless(intent.getBooleanExtra(EXTRA_ON, false))
                return START_NOT_STICKY
            }
            ACTION_SCAN -> {
                // "Turn on and scan": the user asked for wireless mode too.
                if (!wirelessOn) setWireless(true)
                if (wirelessOn) startScan()
                return START_NOT_STICKY
            }
            ACTION_QR_PAIR -> {
                // A scanned MyCam code: Wi-Fi mode on (the PC connects to it), then pair and connect.
                val text = intent.getStringExtra(EXTRA_QR) ?: return START_NOT_STICKY
                if (!wirelessOn) setWireless(true)
                if (wirelessOn) startQrPair(text)
                return START_NOT_STICKY
            }
            ACTION_CONNECT_PC -> {
                intent.getStringExtra(EXTRA_PC_IP)?.let { askPcToConnect(it) }
                return START_NOT_STICKY
            }
            ACTION_FORGET_PCS -> {
                val id = intent.getStringExtra(EXTRA_PC_ID)
                if (id != null) forgetPc(id) else forgetPcs()
                return START_NOT_STICKY
            }
            ACTION_CAMERA_GATE -> {
                answerCameraGate(intent.getBooleanExtra(EXTRA_ALLOW, false))
                return START_NOT_STICKY
            }
            ACTION_WIRELESS_ANSWER -> {
                answerPc(intent.getBooleanExtra(EXTRA_ALLOW, false))
                return START_NOT_STICKY
            }
        }

        val acc = intent?.let { IntentCompat.getParcelableExtra(it, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java) }
        if (!goForeground()) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (acc != null && acc != accessory) openAccessory(acc)
        else if (!linked && !wirelessOn) stopSelf()
        return START_NOT_STICKY
    }

    // --- Wireless mode (IMPROVEMENTS.md 11) ----------------------------------------------------------

    /** Main thread. Turns wireless mode on (findable on Wi-Fi, accepting PCs) or off. */
    private fun setWireless(on: Boolean) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_WIRELESS, on).apply()
        if (on == wirelessOn) return
        if (on) {
            if (!goForeground()) { stopSelf(); return }
            wirelessOn = true
            wireless = WirelessServer(
                this,
                onProbe = { ip, port, name -> main.post { onProbe(ip, port, name) } },
                onClient = { sock, name -> main.post { offerPc(sock, name) } },
                onRefusal = { ip, reason -> main.post { onQrRefused(ip, reason) } },
            ).also { it.start() }
            _state.update {
                it.copy(wirelessOn = true, wirelessAddress = WirelessServer.localAddress(this), pairedPcs = pairedPcs.list())
            }
        } else {
            wirelessOn = false
            wireless?.stop()
            wireless = null
            answerPc(false)
            main.removeCallbacks(endScan)
            main.removeCallbacks(expireNearby)
            stopInviting()
            qrPending = null // A QR pairing needs wireless mode.
            main.removeCallbacks(qrTick)
            nearby.clear()
            _state.update {
                it.copy(wirelessOn = false, wirelessAddress = null, nearbyPcs = emptyList(), scanning = false, scanned = false, qr = null)
            }
            if (socket != null) {
                val id = linkId
                camera.post { onLinkLost(id) }
            } else if (!linked) {
                stopSelf()
            }
        }
        updateNotification()
    }

    /**
     * Main thread. "Scan for PCs": the phone never probes (PROTOCOL.md "Wireless transport"), so a scan is
     * listening for three of the PCs' 2 s probes while the UI shows progress. The nearby list itself is kept
     * all the time wireless mode is on. Also refreshes this phone's address (it may have changed networks).
     */
    private fun startScan() {
        main.removeCallbacks(endScan)
        publishNearby()
        _state.update { it.copy(scanning = true, wirelessAddress = WirelessServer.localAddress(this)) }
        main.postDelayed(endScan, NearbyPcs.MAX_AGE_MS)
    }

    /**
     * Main thread. The user picked a PC from the nearby list: ask it to connect to this phone (PROTOCOL.md
     * "The phone asks for a session"), so a pairing can start from either screen. The PC does the rest -
     * it opens the TCP session and runs the handshake, and the 6-digit code appears on both.
     */
    private fun askPcToConnect(ip: String) {
        if (!wirelessOn || linked || handshaking) return
        val pc = nearby.find(ip, SystemClock.elapsedRealtime())
        if (pc == null || pc.port == 0) {
            Log.w(TAG, "can't ask $ip to connect: it isn't in the nearby list any more")
            return
        }
        main.removeCallbacks(resendInvite)
        invitingIp = ip
        invitesLeft = INVITE_TRIES
        _state.update { it.copy(invitingPc = ip) }
        Log.i(TAG, "asking ${pc.name} ($ip:${pc.port}) to connect")
        resendInvite.run()
    }

    /**
     * Main thread. The user scanned the PC's QR code (PROTOCOL.md "QR pairing"). The PC probes while its code
     * shows; once its probe is heard, this phone answers with an invite that proves it holds the code, and
     * the PC connects ([offerPc]) with the code in the handshake's keys. No code to compare and nothing to
     * allow: the scan was the consent. [startLink] ends it; [finishQr] says why if it didn't work.
     */
    private fun startQrPair(text: String) {
        val code = (QrPairing.parse(text) as? QrPairing.Scan.Ok)?.code ?: return
        // Already connected to that PC over Wi-Fi (it was scanned again): nothing to do.
        if (linked && _state.value.wireless && _state.value.wirelessPcIp == code.host) {
            _state.update { it.copy(qr = null) }
            return
        }
        qrPending = QrPending(code, SystemClock.elapsedRealtime() + QR_TIMEOUT_MS)
        _state.update { it.copy(qr = QrStatus()) }
        main.removeCallbacks(qrTick)
        qrTick.run()
    }

    /** Main thread. The QR pairing is over: connected (null), or why not. */
    private fun finishQr(failure: QrPairing.Failure?) {
        val p = qrPending
        qrPending = null
        main.removeCallbacks(qrTick)
        _state.update { it.copy(qr = if (failure == null) null else QrStatus(p?.pcName, failure)) }
    }

    /**
     * Main thread. The PC we invited said no: our code isn't its current one, or another phone has it.
     *
     * This is a plain datagram with nothing to prove it came from that PC, and a source address is easy to
     * forge on a local network, so it is a hint rather than an answer: the reason is shown and remembered,
     * and the phone keeps inviting until the attempt runs out. One forged datagram then costs a few seconds
     * instead of ending the pairing with a message telling the user to go and make a new code.
     */
    private fun onQrRefused(ip: String, reason: String) {
        val p = qrPending ?: return
        if (p.code.host != ip || handshaking || linked) return
        // Only right after an invite of our own: otherwise it answers nothing we asked.
        if (p.invitedAt == 0L || SystemClock.elapsedRealtime() - p.invitedAt > QR_REFUSAL_WINDOW_MS) return
        val failure = if (reason == "busy") QrPairing.Failure.Busy else QrPairing.Failure.CodeUsed
        if (p.refused == failure) return
        p.refused = failure
        _state.update { it.copy(qr = QrStatus(p.pcName, failure, trying = true)) }
    }

    /** Handshake thread. The code scanned from that PC's screen, while a QR pairing with it is going on. */
    private fun qrCodeFor(pcId: ByteArray): ByteArray? =
        qrPending?.takeIf { it.code.pcId.contentEquals(pcId) && SystemClock.elapsedRealtime() < it.until }?.code?.code

    /** Main thread. Stops asking a PC to connect (it did, it never answered, or wireless mode went off). */
    private fun stopInviting() {
        main.removeCallbacks(resendInvite)
        invitingIp = null
        invitesLeft = 0
        if (_state.value.invitingPc != null) _state.update { it.copy(invitingPc = null) }
    }

    /** Main thread. A PC's discovery probe arrived. */
    private fun onProbe(ip: String, port: Int, name: String) {
        if (!wirelessOn) return
        val wasEmpty = nearby.isEmpty()
        if (nearby.seen(ip, name, SystemClock.elapsedRealtime(), port)) publishNearby()
        if (wasEmpty) {
            main.removeCallbacks(expireNearby)
            main.postDelayed(expireNearby, 1_000)
        }
        // The PC whose QR code was scanned is probing: invite it now rather than at the next tick.
        qrPending?.let { p ->
            if (p.code.host == ip && !p.heard) {
                main.removeCallbacks(qrTick)
                qrTick.run()
            }
        }
    }

    /** Main thread. Shows the current nearby PCs (only when the list changed, not on every probe). */
    private fun publishNearby() {
        val list = nearby.list(SystemClock.elapsedRealtime())
        if (list != _state.value.nearbyPcs) _state.update { it.copy(nearbyPcs = list) }
    }

    /**
     * Main thread. A PC connected over Wi-Fi. Unless USB is in use or another PC is being set up, run the
     * handshake on its own thread: a paired PC connects straight away; a new one is paired only if the user
     * allows it after comparing the 6-digit code (PROTOCOL.md "Wireless security").
     */
    private fun offerPc(sock: Socket, discoveredName: String) {
        if (!wirelessOn || linked || handshaking) {
            // The cable wins, and only one PC at a time.
            try { sock.close() } catch (_: IOException) {}
            return
        }
        handshaking = true
        stopInviting() // A PC turned up: whether or not it is the one we asked, the asking is over.
        Thread({
            val result = WifiHandshake(
                sock, pairedPcs, pairedPcs.phoneId(), WirelessServer.deviceName(this), ::askUser, ::qrCodeFor,
            ).run()
            main.post {
                handshaking = false
                if (result != null && wirelessOn && !linked) openSocket(sock, result)
                else try { sock.close() } catch (_: IOException) {}
            }
        }, "wifi-handshake").start()
        Log.i(TAG, "Wi-Fi connection from $discoveredName (${sock.inetAddress.hostAddress})")
    }

    /** Handshake thread. Shows "Pair with <PC>? Code …" and waits for Allow / Don't allow (or the timeout). */
    private fun askUser(pcName: String, code: String, pcGone: () -> Boolean): Boolean {
        val latch = java.util.concurrent.CountDownLatch(1)
        val answer = java.util.concurrent.atomic.AtomicBoolean(false)
        main.post {
            pendingAnswer = { allow -> answer.set(allow); latch.countDown() }
            val deadline = android.os.SystemClock.elapsedRealtime() + APPROVAL_TIMEOUT_MS
            _state.update { it.copy(pendingPc = pcName, pendingCode = code, pendingDeadline = deadline, pairingTimedOut = null) }
            showApprovalNotification(pcName, code)
        }
        // Wait for the answer, the timeout, or the PC hanging up (Cancel pairing on the PC).
        val giveUp = android.os.SystemClock.elapsedRealtime() + APPROVAL_TIMEOUT_MS
        var answered = false
        while (!answered && !pcGone()) {
            val left = giveUp - android.os.SystemClock.elapsedRealtime()
            if (left <= 0) break
            answered = latch.await(minOf(left, 250L), java.util.concurrent.TimeUnit.MILLISECONDS)
        }
        val cancelled = !answered && pcGone()
        main.post {
            // Unanswered: refuse, and let the dialog say "Pairing timed out" instead of vanishing (unless the
            // PC cancelled: then it just closes).
            if (!answered && pendingAnswer != null) {
                answerPc(false)
                if (!cancelled) _state.update { it.copy(pairingTimedOut = pcName) }
            }
        }
        return answered && answer.get()
    }

    /** Main thread. The user allowed or refused the PC that is pairing. */
    private fun answerPc(allow: Boolean) {
        val reply = pendingAnswer ?: return
        pendingAnswer = null
        _state.update { it.copy(pendingPc = null, pendingCode = null, pendingDeadline = 0L) }
        getSystemService(NotificationManager::class.java).cancel(APPROVAL_NOTIFICATION_ID)
        reply(allow && wirelessOn && !linked)
    }

    /** Main thread. Forgets every paired PC: they have to pair again (with a code) next time. */
    private fun forgetPcs() {
        pairedPcs.forgetAll()
        _state.update { it.copy(pairedPcs = emptyList()) }
        if (!linked && !wirelessOn) stopSelf() // Started just for this.
    }

    /** Main thread. Forgets one paired PC (A11). A PC connected right now stays connected until it leaves. */
    private fun forgetPc(id: String) {
        pairedPcs.forget(id)
        _state.update { it.copy(pairedPcs = pairedPcs.list()) }
        if (!linked && !wirelessOn) stopSelf()
    }

    private fun showApprovalNotification(name: String, code: String) {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(APPROVAL_CHANNEL_ID, getString(R.string.channel_wireless), NotificationManager.IMPORTANCE_HIGH)
            )
        }
        fun answer(allow: Boolean, requestCode: Int) = PendingIntent.getService(
            this, requestCode, Intent(this, WebcamService::class.java).setAction(ACTION_WIRELESS_ANSWER).putExtra(EXTRA_ALLOW, allow),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val open = PendingIntent.getActivity(this, 2, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        nm.notify(
            APPROVAL_NOTIFICATION_ID,
            NotificationCompat.Builder(this, APPROVAL_CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_webcam)
                .setContentTitle(getString(R.string.wireless_pair_title, name))
                .setContentText(getString(R.string.wireless_pair_text, formatCode(code)))
                .setStyle(NotificationCompat.BigTextStyle().bigText(getString(R.string.wireless_pair_text, formatCode(code))))
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setCategory(NotificationCompat.CATEGORY_CALL)
                .setContentIntent(open)
                .setAutoCancel(true)
                .setTimeoutAfter(APPROVAL_TIMEOUT_MS)
                .addAction(0, getString(R.string.wireless_deny), answer(false, 3))
                .addAction(0, getString(R.string.wireless_allow), answer(true, 4))
                .build(),
        )
    }

    /** Main thread. Uses a paired, encrypted Wi-Fi connection as the link. */
    private fun openSocket(sock: Socket, secure: WifiHandshake.Result) {
        closeLink()
        try {
            sock.tcpNoDelay = true
            // A small send buffer keeps delay low: a big one hides a slow network from the adaptive bitrate
            // (writes return at once while seconds of video queue up behind them).
            sock.sendBufferSize = 128 * 1024
        } catch (_: IOException) {}
        socket = sock
        // Pairing says the two sides share a key; it doesn't say this PC may watch. Until the user allows it,
        // the session runs with the camera off (START is answered with STATE(paused), as a user pause is).
        cameraBlocked = !pairedPcs.cameraAllowed(secure.pcId)
        gatedPcId = if (cameraBlocked) secure.pcId else null
        startLink(secure.output, secure.input, wirelessPc = secure.pcName)
        if (cameraBlocked) _state.update { it.copy(cameraGate = secure.pcName) }
        _state.update { it.copy(wirelessPcIp = sock.inetAddress?.hostAddress) }
        // A new pairing (QR code or 6-digit code): the screen says so and shows the camera. When the camera is
        // still waiting to be allowed, the gate asks and names the PC, so there is no second message.
        if (secure.newlyPaired) {
            _state.update {
                it.copy(pairedPcs = pairedPcs.list(), justPaired = if (cameraBlocked) null else secure.pcName)
            }
        }
        wifiLock = getSystemService(WifiManager::class.java)?.createWifiLock(
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            else @Suppress("DEPRECATION") WifiManager.WIFI_MODE_FULL_HIGH_PERF,
            "MyCam:stream",
        )?.apply { setReferenceCounted(false); acquire() }
        Log.i(TAG, "Wi-Fi link to ${secure.pcName} (${sock.inetAddress.hostAddress}), encrypted")
    }


    private fun goForeground(): Boolean {
        if (ContextCompat.checkSelfPermission(this, android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "Camera permission missing; cannot start")
            return false
        }
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
            )
        }
        val notification = buildNotification()
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            foreground = true
            true
        } catch (e: Exception) {
            Log.e(TAG, "startForeground failed", e)
            false
        }
    }

    /** Ongoing notification with a Pause / Resume action, so the camera can be paused from the shade. */
    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val toggle = PendingIntent.getService(
            this, 1,
            Intent(this, WebcamService::class.java).setAction(ACTION_SET_PAUSED).putExtra(EXTRA_PAUSED, !paused),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_webcam)
            .setContentTitle(getString(when { paused -> R.string.notification_title_paused; !linked && wirelessOn -> R.string.notification_title_wireless; else -> R.string.notification_title }))
            .setContentText(getString(when { paused -> R.string.notification_text_paused; !linked && wirelessOn -> R.string.notification_text_wireless; else -> R.string.notification_text }))
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(
                if (paused) R.drawable.ic_action_resume else R.drawable.ic_action_pause,
                getString(if (paused) R.string.action_resume else R.string.action_pause),
                toggle,
            )
            .build()
    }

    private fun updateNotification() {
        if (!foreground) return
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification())
    }

    private fun openAccessory(acc: UsbAccessory) {
        // The cable wins over Wi-Fi: drop a wireless link (and any PC waiting for approval).
        answerPc(false)
        closeLink()
        val usb = getSystemService(UsbManager::class.java)
        val fd = try { usb.openAccessory(acc) } catch (e: SecurityException) { null }
        if (fd == null) {
            _state.update { it.copy(error = getString(R.string.error_open_accessory)) }
            if (!wirelessOn) stopSelf()
            return
        }
        accessory = acc
        pfd = fd
        startLink(FileOutputStream(fd.fileDescriptor), FileInputStream(fd.fileDescriptor), wirelessPc = null)
    }

    /** Main thread. Common start of a USB or Wi-Fi link: reader thread, sensors, wake lock, UI state. */
    private fun startLink(out: OutputStream, input: InputStream, wirelessPc: String?) {
        val id = ++linkId
        output = out
        linked = true
        if (wirelessPc == null) { // Over the cable, plugging it in is the consent.
            cameraBlocked = false
            gatedPcId = null
        }
        qrPending = null // A QR pairing ends here: connected.
        main.removeCallbacks(qrTick)
        _state.update { it.copy(connected = true, error = null, wireless = wirelessPc != null, wirelessPc = wirelessPc, qr = null) }
        orientationListener?.takeIf { it.canDetectOrientation() }?.enable()
        acquireWakeLock()
        updateNotification()
        readerThread = Thread({ readLoop(input, id) }, if (wirelessPc != null) "wifi-reader" else "accessory-reader").apply { start() }
    }

    private fun readLoop(input: InputStream, id: Int) {
        // AOA reads must use a buffer of at least 16 KiB or some kernels fail the transfer.
        val buf = ByteArray(16384)
        val parser = Protocol.CommandParser()
        try {
            while (!Thread.currentThread().isInterrupted) {
                val n = input.read(buf)
                if (n < 0) break
                for (c in parser.feed(buf, n)) camera.post { handleCommand(c.cmd, c.arg) }
            }
        } catch (e: IOException) {
            Log.i(TAG, "Link read ended: ${e.message}")
        }
        // The PC side went away (companion closed, cable pulled or Wi-Fi lost).
        camera.post { onLinkLost(id) }
    }

    /**
     * Camera thread. The link [id] ended: turn the camera off and close it. The service stays up while
     * wireless mode is on (a PC may connect again); otherwise it stops, as before.
     */
    private fun onLinkLost(id: Int) {
        if (id != linkId || !linked) return
        wantStreaming = false
        stopStreamer()
        main.post {
            if (id != linkId) return@post
            closeLink()
            if (!wirelessOn) stopSelf()
        }
    }

    private fun handleCommand(cmd: Int, arg: Int) {
        remoteLog("command $cmd (arg $arg)")
        when (cmd) {
            Protocol.CMD_HELLO -> {
                send(Protocol.TYPE_HELLO, 0, 0, byteArrayOf(0, Protocol.VERSION.toByte()))
                sendState()
                sendOrientation()
                sendCameraInfo()
            }
            Protocol.CMD_START -> {
                wantStreaming = true
                // Blocked or paused, the answer is the same: stay off and say so.
                if (paused || cameraBlocked) sendState() // The PC shows its "Camera paused" picture.
                else if (streamer == null) startStreamer()
                else {
                    lastConfig?.let { send(Protocol.TYPE_CONFIG, 0, 0, it) }
                    streamer?.requestKeyFrame()
                }
            }
            Protocol.CMD_STOP -> {
                wantStreaming = false
                stopStreamer()
            }
            Protocol.CMD_KEYFRAME -> {
                lastConfig?.let { send(Protocol.TYPE_CONFIG, 0, 0, it) }
                streamer?.requestKeyFrame()
            }
            Protocol.CMD_SET_FACING -> setFacing(arg)
            Protocol.CMD_PAUSE -> setPaused(true)
            Protocol.CMD_RESUME -> setPaused(false)
            Protocol.CMD_SET_QUALITY -> changeVideo(camSettings.copy(quality = arg.coerceIn(Protocol.QUALITY_720P, Protocol.QUALITY_4K)))
            Protocol.CMD_SET_FPS -> changeVideo(camSettings.copy(fps = if (arg >= 120) 120 else if (arg >= 60) 60 else 30))
            Protocol.CMD_SET_ZOOM -> changeControls(camSettings.copy(zoom = arg / 10f))
            Protocol.CMD_SET_EXPOSURE -> changeControls(camSettings.copy(ev = arg.toByte().toInt()))
            Protocol.CMD_SET_TORCH -> changeControls(camSettings.copy(torch = arg != 0))
            Protocol.CMD_SET_FOCUS -> changeControls(camSettings.copy(focusLocked = arg != 0))
        }
    }

    /**
     * Main thread. The user answered "Let this PC use the camera?". Allow remembers it for that PC and starts
     * the camera if the PC is already asking for video; Don't allow forgets the PC and drops the link, so a
     * code scanned by mistake (or off someone else's screen) leaves nothing behind.
     */
    private fun answerCameraGate(allow: Boolean) {
        val pcId = gatedPcId ?: return
        gatedPcId = null // Answered either way: a second tap does nothing.
        _state.update { it.copy(cameraGate = null) }
        if (!allow) {
            Log.i(TAG, "camera not allowed for this PC: forgetting it and dropping the link")
            pairedPcs.forget(QrPairing.hex(pcId))
            _state.update { it.copy(pairedPcs = pairedPcs.list()) }
            val id = linkId
            camera.post { onLinkLost(id) }
            return
        }
        pairedPcs.allowCamera(pcId)
        _state.update { it.copy(pairedPcs = pairedPcs.list()) }
        camera.post {
            cameraBlocked = false
            if (wantStreaming && !paused && streamer == null) startStreamer() else sendState()
        }
    }

    /** Camera thread. Pausing turns the camera fully off; resuming restarts it only if the PC still wants video. */
    private fun setPaused(p: Boolean) {
        if (p != paused) {
            paused = p
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_PAUSED, p).apply()
            _state.update { it.copy(paused = p) }
            updateNotification()
            remoteLog(if (p) "paused" else "resumed")
        }
        when {
            p && streamer != null -> stopStreamer() // Reports STATE_PAUSED.
            !p && wantStreaming && streamer == null -> startStreamer()
            else -> sendState()
        }
    }

    /** Quality or frame rate changed: save, and restart the camera if it is running. */
    private fun changeVideo(s: CameraStreamer.Settings) {
        if (s == camSettings) return
        camSettings = s
        saveCameraSettings(getSharedPreferences(PREFS, MODE_PRIVATE), s)
        _state.update { it.copy(camera = s) }
        if (streamer != null) {
            stopStreamer()
            startStreamer()
        } else {
            sendCameraInfo()
        }
    }

    /** Zoom, exposure, torch or focus changed: save, and apply live without restarting. */
    private fun changeControls(s: CameraStreamer.Settings) {
        camSettings = s
        saveCameraSettings(getSharedPreferences(PREFS, MODE_PRIVATE), s)
        _state.update { it.copy(camera = s) }
        streamer?.updateControls(s) ?: sendCameraInfo()
    }

    /** Tells the PC the current settings (and, once known, what the camera supports). */
    private fun sendCameraInfo() {
        val c = camSettings
        val info = lastCameraInfo?.copy(
            quality = c.quality, fps = c.fps, zoomX100 = (c.zoom * 100).toInt().coerceIn(lastCameraInfo!!.zoomMinX100, lastCameraInfo!!.zoomMaxX100),
            ev = c.ev.coerceIn(lastCameraInfo!!.evMin, lastCameraInfo!!.evMax),
            flags = (lastCameraInfo!!.flags and (Protocol.CAM_TORCH_ON or Protocol.CAM_FOCUS_LOCKED).inv()) or
                (if (c.torch && lastCameraInfo!!.flags and Protocol.CAM_TORCH_AVAILABLE != 0) Protocol.CAM_TORCH_ON else 0) or
                (if (c.focusLocked && lastCameraInfo!!.flags and Protocol.CAM_HAS_AUTOFOCUS != 0) Protocol.CAM_FOCUS_LOCKED else 0),
        )
            // No camera opened yet, so the ranges are placeholders (zoom 1-1, EV 0-0): report values inside them,
            // not a saved zoom/EV outside them (#16). The real ones follow once the camera opens.
            ?: Protocol.CameraInfo(c.quality, c.fps, 100, 100, 100, 0, 0, 0, 0, 0, 0, 0, 0)
        send(Protocol.TYPE_CAMERA, 0, 0, info.encode())
    }

    private fun setFacing(f: Int) {
        if (f != Protocol.FACING_BACK && f != Protocol.FACING_FRONT) return
        facing = f
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FACING, f).apply()
        _state.update { it.copy(facing = f) }
        if (streamer != null) {
            stopStreamer()
            startStreamer()
        } else {
            sendState()
        }
    }

    private fun startStreamer() {
        if (!linked) return
        remoteLog("starting ${if (facing == Protocol.FACING_FRONT) "front" else "back"} camera")
        framesSent = 0
        bytesSent = 0L
        val s = CameraStreamer(this, camera, facing, camSettings, object : CameraStreamer.Listener {
            override fun onLog(message: String) = remoteLog(message)

            override fun onPreviewSupport(support: PreviewSupport) = PhonePreview.reportSupport(support)

            override fun onCameraInfo(info: Protocol.CameraInfo) {
                lastCameraInfo = info
                _state.update { it.copy(cameraInfo = info) }
                sendCameraInfo()
            }

            override fun onConfig(width: Int, height: Int, sensorOrientation: Int, facing: Int, csd: ByteArray) {
                val payload = ByteBuffer.allocate(7 + csd.size).order(ByteOrder.BIG_ENDIAN)
                    .putShort(width.toShort()).putShort(height.toShort())
                    .putShort(sensorOrientation.toShort()).put(facing.toByte()).put(csd).array()
                lastConfig = payload
                send(Protocol.TYPE_CONFIG, 0, 0, payload)
                _state.update { it.copy(streaming = true, resolution = "${width}×$height", error = null) }
                sendState()
            }

            override fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) {
                // Behind: frames are skipped until the next key frame, so the backlog never builds up
                // (H.264 can't drop single frames without breaking the picture until the next key frame).
                if (dropUntilKey) {
                    if (!keyFrame) { framesDropped++; return }
                    dropUntilKey = false
                }
                val t0 = SystemClock.elapsedRealtime()
                send(Protocol.TYPE_FRAME, if (keyFrame) Protocol.FLAG_KEYFRAME else 0, ptsUs, data)
                adaptToLink(SystemClock.elapsedRealtime() - t0, keyFrame)
                framesSent++
                bytesSent += data.size
                val now = SystemClock.elapsedRealtime()
                if (framesSent == 1 || now - lastStatsLog > 5000) {
                    remoteLog("sent $framesSent frames (${bytesSent / 1024} KB), dropped $framesDropped, bitrate ${linkBitrate / 1_000_000} Mbps")
                    lastStatsLog = now
                }
            }

            override fun onError(message: String) {
                Log.e(TAG, message)
                remoteLog("error: $message")
                stopStreamer()
                _state.update { it.copy(error = message) }
                send(Protocol.TYPE_STATE, 0, 0, byteArrayOf(Protocol.STATE_ERROR.toByte()))
            }
        })
        streamer = s
        linkBitrate = 0 // Set from the streamer's target on the first frame.
        linkCeiling = Int.MAX_VALUE
        dropUntilKey = false
        framesDropped = 0
        slowestWriteMs = 0
        lastBitrateChange = SystemClock.elapsedRealtime()
        // Whatever the screen is asking for right now; it is attached once the session is configured.
        s.setPreviewSurface(PhonePreview.target())
        s.start()
    }

    private fun stopStreamer() {
        streamer?.stop()
        streamer = null
        PhonePreview.cameraStopped()
        lastConfig = null
        _state.update { it.copy(streaming = false, resolution = "") }
        sendState()
    }

    /**
     * A frame write that takes longer than its time slot means the USB link (the PC reads as fast as it can)
     * is carrying less than the encoder produces. Then: skip to the next key frame and lower the bitrate by
     * a quarter. When writes stay fast for 5 s, raise the bitrate back towards the mode's target, but not
     * above 90% of a bitrate the link already fell behind at (climbing back there just repeats the stall).
     * The ceiling is never below twice the floor and expires after 30 s: a stall can come from a busy PC
     * rather than the link, and a ceiling at the floor would pin the stream at the worst quality (ISSUES.md 1).
     * The arithmetic is in Long: 4K bitrates times 115 overflow Int, which once sent the encoder a
     * negative bitrate and made the picture collapse every ~30 s.
     */
    private fun adaptToLink(writeMs: Long, keyFrame: Boolean) {
        val s = streamer ?: return
        if (linkBitrate == 0) linkBitrate = s.targetBitrate
        val slot = s.frameIntervalMs.toLong()
        val now = SystemClock.elapsedRealtime()
        if (linkCeiling != Int.MAX_VALUE && now - linkCeilingSince > CEILING_EXPIRY_MS) linkCeiling = Int.MAX_VALUE
        // Key frames are several times larger, so they get more room before counting as "behind".
        if (writeMs > slot * (if (keyFrame) 4 else 2) && now - lastBitrateChange > 1000) {
            linkCeiling = minOf(linkCeiling, (linkBitrate.toLong() * 9 / 10).toInt()).coerceAtLeast(MIN_BITRATE * 2)
            linkCeilingSince = now
            linkBitrate = (linkBitrate.toLong() * 3 / 4).toInt().coerceAtLeast(MIN_BITRATE)
            s.setBitrate(linkBitrate)
            dropUntilKey = true
            s.requestKeyFrame()
            remoteLog("link behind (frame write took $writeMs ms): bitrate now ${linkBitrate / 1000} kbps, skipping to next key frame")
            lastBitrateChange = now
            slowestWriteMs = 0
            return
        }
        slowestWriteMs = maxOf(slowestWriteMs, if (keyFrame) writeMs / 4 else writeMs)
        val limit = minOf(s.targetBitrate, linkCeiling)
        if (now - lastBitrateChange > 5000 && linkBitrate < limit) {
            if (slowestWriteMs < slot / 2) {
                linkBitrate = (linkBitrate.toLong() * 115 / 100).coerceAtMost(limit.toLong()).toInt()
                s.setBitrate(linkBitrate)
                remoteLog("link has room: bitrate now ${linkBitrate / 1000} kbps")
            }
            lastBitrateChange = now
            slowestWriteMs = 0
        }
    }

    private fun sendState() {
        val st = when {
            paused || cameraBlocked -> Protocol.STATE_PAUSED
            streamer != null -> Protocol.STATE_STREAMING
            else -> Protocol.STATE_IDLE
        }
        send(Protocol.TYPE_STATE, 0, 0, byteArrayOf(st.toByte(), facing.toByte()))
    }

    private fun sendOrientation() {
        send(Protocol.TYPE_ORIENT, 0, 0, byteArrayOf((deviceRotation shr 8).toByte(), deviceRotation.toByte()))
    }

    /** Logs locally and forwards the text to the PC log (mycam.log). Camera thread only. */
    private fun remoteLog(message: String) {
        Log.i(TAG, message)
        send(Protocol.TYPE_LOG, 0, 0, message.toByteArray())
    }

    /** Called on the camera thread only, so writes are naturally serialized. */
    private fun send(type: Int, flags: Int, ptsUs: Long, payload: ByteArray) {
        val out = output ?: return
        try {
            out.write(Protocol.packet(type, flags, ptsUs, payload))
        } catch (e: IOException) {
            Log.i(TAG, "Accessory write failed: ${e.message}")
            output = null
            stopStreamer()
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MyCam:link")
                .apply { setReferenceCounted(false); acquire() }
        }
    }

    /** Closes the current link (USB or Wi-Fi), if any. */
    private fun closeLink() {
        linked = false
        readerThread?.interrupt()
        readerThread = null
        try { pfd?.close() } catch (_: IOException) {}
        try { socket?.close() } catch (_: IOException) {}
        pfd = null
        socket = null
        output = null
        accessory = null
        wifiLock?.release()
        wifiLock = null
        // Held for a link only: with wireless mode on the service outlives the link, and the phone must sleep.
        wakeLock?.release()
        wakeLock = null
        orientationListener?.disable()
        cameraBlocked = false
        gatedPcId = null
        _state.update {
            it.copy(connected = false, streaming = false, resolution = "", wireless = false, wirelessPc = null,
                    wirelessPcIp = null, cameraGate = null)
        }
        updateNotification()
    }

    /**
     * The user swiped MyCam away from the recent apps: that means "quit", so the camera, the link and wireless
     * mode all stop (onDestroy does the work; the PC sees the link end). The wireless choice stays saved, so
     * opening MyCam again turns it back on, and plugging the cable in again starts as usual.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        Log.i(TAG, "removed from recent apps: stopping")
        stopForeground(STOP_FOREGROUND_REMOVE)
        foreground = false
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        getSystemService(NotificationManager::class.java).cancel(APPROVAL_NOTIFICATION_ID)
        unregisterReceiver(detachReceiver)
        PhonePreview.onTargetChanged = null
        PhonePreview.cameraStopped()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            (thermalListener as? PowerManager.OnThermalStatusChangedListener)
                ?.let { getSystemService(PowerManager::class.java).removeThermalStatusListener(it) }
        }
        orientationListener?.disable()
        camera.post {
            streamer?.stop()
            streamer = null
        }
        cameraThread.quitSafely()
        wireless?.stop()
        wireless = null
        main.removeCallbacks(endScan)
        main.removeCallbacks(expireNearby)
        main.removeCallbacks(resendInvite)
        main.removeCallbacks(qrTick)
        pendingAnswer?.invoke(false) // Unblocks a pairing that is waiting for the user.
        pendingAnswer = null
        closeLink()
        _state.update { UiState(facing = facing, paused = paused, camera = camSettings, pairedPcs = pairedPcs.list()) }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "WebcamService"
        private const val CHANNEL_ID = "webcam"
        private const val NOTIFICATION_ID = 1
        private const val MIN_BITRATE = 2_000_000
        private const val CEILING_EXPIRY_MS = 30_000L
        const val PREFS = "mycam"
        const val PREF_FACING = "facing"
        const val ACTION_SET_FACING = "io.github.whoissaaif.mycam.SET_FACING"
        const val EXTRA_FACING = "facing"
        const val PREF_PAUSED = "paused"
        const val ACTION_COMMAND = "io.github.whoissaaif.mycam.COMMAND"
        const val EXTRA_CMD = "cmd"
        const val EXTRA_ARG = "arg"
        const val PREF_WIRELESS = "wireless"
        const val ACTION_WIRELESS = "io.github.whoissaaif.mycam.WIRELESS"
        const val EXTRA_ON = "on"
        const val ACTION_WIRELESS_ANSWER = "io.github.whoissaaif.mycam.WIRELESS_ANSWER"
        const val EXTRA_ALLOW = "allow"
        /** "Scan for PCs": turns wireless mode on if needed, then shows progress for 6 s while PCs are heard. */
        const val ACTION_SCAN = "io.github.whoissaaif.mycam.SCAN"
        /** Asks the nearby PC at EXTRA_PC_IP to connect to this phone, so a pairing can start here. */
        const val ACTION_CONNECT_PC = "io.github.whoissaaif.mycam.CONNECT_PC"
        /** With ACTION_CONNECT_PC: the IP address of the PC to ask, as the nearby list shows it. */
        const val EXTRA_PC_IP = "pc_ip"
        /** A scanned MyCam QR code (EXTRA_QR, the text): turn wireless mode on, pair and connect. */
        const val ACTION_QR_PAIR = "io.github.whoissaaif.mycam.QR_PAIR"
        const val EXTRA_QR = "qr"
        /** How long a QR pairing keeps trying (hearing the PC, then its connection), and the invite resend. */
        private const val QR_TIMEOUT_MS = 20_000L
        private const val QR_RESEND_MS = 2_000L
        /** A "MYCAM-1" is only believed this soon after an invite of ours (see [onQrRefused]). */
        private const val QR_REFUSAL_WINDOW_MS = 4_000L
        const val ACTION_FORGET_PCS ="io.github.whoissaaif.mycam.FORGET_PCS"
        /** The answer to "Let this PC use the camera?" (EXTRA_ALLOW). */
        const val ACTION_CAMERA_GATE = "io.github.whoissaaif.mycam.CAMERA_GATE"
        /** With ACTION_FORGET_PCS: forget only this PC (PairedPc.id); without it, forget them all. */
        const val EXTRA_PC_ID = "pc_id"
        /** Dim the screen automatically while streaming (Settings > Phone). */
        const val PREF_AUTO_DIM = "auto_dim"
        /** The phone's own live preview (docs/redesign-v2.md 6.1). Default on. */
        const val PREF_PREVIEW = "preview"
        /** Set after the first successful connection: the first-run cards stop showing. */
        const val PREF_FIRST_RUN_DONE = "first_run_done"

        /** "554294" -> "554 294", easier to compare between screens. */
        fun formatCode(code: String) = if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code
        private const val APPROVAL_CHANNEL_ID = "wireless"
        private const val APPROVAL_NOTIFICATION_ID = 2
        private const val APPROVAL_TIMEOUT_MS = 60_000L

        /** "Connect to me" is resent every 2 s, five times, because a single datagram may be lost. */
        private const val INVITE_EVERY_MS = 2_000L
        private const val INVITE_TRIES = 5

        fun loadCameraSettings(prefs: android.content.SharedPreferences) = CameraStreamer.Settings(
            quality = prefs.getInt("quality", Protocol.QUALITY_1080P),
            fps = prefs.getInt("fps", 30),
            zoom = prefs.getFloat("zoom", 1f),
            ev = prefs.getInt("ev", 0),
            // Torch and focus lock reset on each connection: a torch left on would surprise people.
        )

        fun saveCameraSettings(prefs: android.content.SharedPreferences, s: CameraStreamer.Settings) {
            prefs.edit().putInt("quality", s.quality).putInt("fps", s.fps).putFloat("zoom", s.zoom).putInt("ev", s.ev).apply()
        }

        /** Shows saved camera settings while no PC is connected. */
        fun showIdleCamera(s: CameraStreamer.Settings) {
            _state.update { if (it.connected) it else it.copy(camera = s) }
        }
        const val ACTION_SET_PAUSED = "io.github.whoissaaif.mycam.SET_PAUSED"
        const val EXTRA_PAUSED = "paused"

        private val _state = MutableStateFlow(UiState())
        val state: StateFlow<UiState> = _state.asStateFlow()

        /** Shows the saved camera choice while no PC is connected. */
        fun showIdleFacing(facing: Int) {
            _state.update { if (it.connected) it else it.copy(facing = facing) }
        }

        /** Shows the saved pause choice while no PC is connected. */
        fun showIdlePaused(paused: Boolean) {
            _state.update { if (it.connected) it else it.copy(paused = paused) }
        }

        /** Shows the paired PCs before the service runs (Settings > Paired PCs). */
        fun showPairedPcs(pcs: List<PairedPc>) {
            _state.update { it.copy(pairedPcs = pcs) }
        }

        /** The user closed the QR pairing message. */
        fun dismissQr() {
            _state.update { it.copy(qr = null) }
        }

        /** The user closed "Paired with ...". */
        fun dismissJustPaired() {
            _state.update { it.copy(justPaired = null) }
        }

        /** The user answered "Let this PC use the camera?" (see [answerCameraGate]). */
        fun answerCameraGate(context: Context, allow: Boolean) {
            context.startService(
                Intent(context, WebcamService::class.java).setAction(ACTION_CAMERA_GATE).putExtra(EXTRA_ALLOW, allow)
            )
        }

        /** The user closed the "Pairing timed out" dialog. */
        fun dismissPairingTimedOut() {
            _state.update { it.copy(pairingTimedOut = null) }
        }

        /** How long a pairing request waits for Allow / Don't allow; the dialog counts this down. */
        const val PAIRING_TIMEOUT_MS = APPROVAL_TIMEOUT_MS
    }
}
