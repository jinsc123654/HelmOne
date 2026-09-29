package com.sifli.sifli_companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.IOException
import java.io.InputStream
import java.util.LinkedHashSet
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Classic RFCOMM / SPP pipe.
 *
 * After GATT is up, [connect] bonds BR/EDR (Just Works) then opens Serial
 * Port UUID 0x1101.  Page scan must be enabled on the MCU; otherwise
 * createBond fails with HCI page timeout (0x04).
 */
class CompanionSppPlugin(private val context: Context) :
    MethodChannel.MethodCallHandler,
    EventChannel.StreamHandler {

    companion object {
        const val METHOD_CHANNEL = "com.sifli.sifli_companion/spp"
        const val EVENT_CHANNEL = "com.sifli.sifli_companion/spp_events"
        private const val TAG = "CompanionSpp"
        private val SPP_UUID: UUID =
            UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
        private const val TRANSPORT_BREDR = 1
        private const val BOND_TIMEOUT_SEC = 35L
    }

    private val io = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager)
            .adapter

    private var socket: BluetoothSocket? = null
    private var reader: Thread? = null
    private var events: EventChannel.EventSink? = null

    @SuppressLint("MissingPermission")
    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "connect" -> {
                val address = call.argument<String>("address")
                if (address.isNullOrBlank()) {
                    result.error("bad_args", "address required", null)
                    return
                }
                io.execute {
                    try {
                        connectLocked(address)
                        main.post { result.success(true) }
                    } catch (e: Exception) {
                        main.post {
                            result.error("connect_failed", e.message, null)
                        }
                    }
                }
            }

            "write" -> {
                val bytes = call.argument<ByteArray>("bytes")
                if (bytes == null) {
                    result.error("bad_args", "bytes required", null)
                    return
                }
                io.execute {
                    try {
                        val s = socket ?: throw IOException("not connected")
                        s.outputStream.write(bytes)
                        s.outputStream.flush()
                        main.post { result.success(true) }
                    } catch (e: Exception) {
                        main.post {
                            result.error("write_failed", e.message, null)
                        }
                    }
                }
            }

            "close" -> {
                io.execute {
                    closeLocked()
                    main.post { result.success(true) }
                }
            }

            "isConnected" -> result.success(socket?.isConnected == true)

            else -> result.notImplemented()
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        this.events = events
    }

    override fun onCancel(arguments: Any?) {
        events = null
    }

    @SuppressLint("MissingPermission")
    private fun connectLocked(address: String) {
        closeLocked()
        val ad = adapter ?: throw IOException("Bluetooth adapter missing")
        if (!ad.isEnabled) {
            throw IOException("Bluetooth is off")
        }

        ad.cancelDiscovery()

        val wanted = address.uppercase()
        val candidates = LinkedHashSet<BluetoothDevice>()
        candidates.add(ad.getRemoteDevice(wanted))
        ad.bondedDevices?.forEach { d ->
            val n = d.name ?: ""
            if (n.startsWith("Helm One-", ignoreCase = true) ||
                n.startsWith("MyBike-", ignoreCase = true) ||
                n.equals("Zephyr", ignoreCase = true) ||
                d.address.equals(wanted, ignoreCase = true)
            ) {
                candidates.add(d)
            }
        }

        var last: Exception? = null
        for (device in candidates) {
            try {
                ensureBonded(device)
                Thread.sleep(400)
                val sock = openRfcomm(device)
                this.socket = sock
                startReader(sock.inputStream)
                return
            } catch (e: Exception) {
                last = e
                closeLocked()
            }
        }
        throw (last ?: IOException("SPP connect failed"))
    }

    @SuppressLint("MissingPermission")
    private fun ensureBonded(device: BluetoothDevice) {
        if (device.bondState == BluetoothDevice.BOND_BONDED) {
            return
        }

        val latch = CountDownLatch(1)
        var bonded = false
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) {
                    return
                }
                val d = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(
                        BluetoothDevice.EXTRA_DEVICE,
                        BluetoothDevice::class.java,
                    )
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                } ?: return
                if (!d.address.equals(device.address, ignoreCase = true)) {
                    return
                }
                when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                    BluetoothDevice.BOND_BONDED -> {
                        bonded = true
                        latch.countDown()
                    }
                    BluetoothDevice.BOND_NONE -> latch.countDown()
                }
            }
        }

        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(receiver, filter)
        }

        try {
            emitPairing(device.address)
            val started = createBondBrEdr(device)
            if (!started && device.bondState != BluetoothDevice.BOND_BONDING &&
                device.bondState != BluetoothDevice.BOND_BONDED
            ) {
                throw IOException("createBond refused")
            }
            if (device.bondState == BluetoothDevice.BOND_BONDED) {
                return
            }
            if (!latch.await(BOND_TIMEOUT_SEC, TimeUnit.SECONDS)) {
                throw IOException("Classic 配对超时（page timeout 或未点配对框）")
            }
            if (!bonded && device.bondState != BluetoothDevice.BOND_BONDED) {
                throw IOException("Classic 配对未完成（取消或失败）")
            }
        } finally {
            try {
                context.unregisterReceiver(receiver)
            } catch (_: Exception) {
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun createBondBrEdr(device: BluetoothDevice): Boolean {
        return try {
            val m = BluetoothDevice::class.java.getMethod(
                "createBond",
                Int::class.javaPrimitiveType,
            )
            m.invoke(device, TRANSPORT_BREDR) as Boolean
        } catch (_: Exception) {
            device.createBond()
        }
    }

    private fun emitPairing(address: String) {
        main.post {
            events?.success(mapOf("type" to "pairing", "address" to address))
        }
    }

    @SuppressLint("MissingPermission")
    private fun openRfcomm(device: BluetoothDevice): BluetoothSocket {
        val errors = mutableListOf<Exception>()
        try {
            val sock = device.createInsecureRfcommSocketToServiceRecord(SPP_UUID)
            sock.connect()
            Log.i(TAG, "RFCOMM insecure UUID 0x1101 ok ${device.address}")
            return sock
        } catch (e: Exception) {
            Log.w(TAG, "insecure UUID failed: ${e.message}")
            errors.add(e)
        }
        try {
            val m = device.javaClass.getMethod(
                "createRfcommSocket",
                Int::class.javaPrimitiveType,
            )
            val sock = m.invoke(device, 5) as BluetoothSocket
            sock.connect()
            Log.i(TAG, "RFCOMM channel 5 ok ${device.address}")
            return sock
        } catch (e: Exception) {
            Log.w(TAG, "channel 5 failed: ${e.message}")
            errors.add(e)
        }
        try {
            val sock = device.createRfcommSocketToServiceRecord(SPP_UUID)
            sock.connect()
            Log.i(TAG, "RFCOMM secure UUID 0x1101 ok ${device.address}")
            return sock
        } catch (e: Exception) {
            Log.w(TAG, "secure UUID failed: ${e.message}")
            errors.add(e)
        }
        throw (errors.lastOrNull() ?: IOException("RFCOMM open failed"))
    }

    private fun startReader(input: InputStream) {
        reader = Thread {
            val buf = ByteArray(4096)
            try {
                while (!Thread.currentThread().isInterrupted) {
                    val n = input.read(buf)
                    if (n < 0) break
                    if (n == 0) continue
                    val chunk = buf.copyOf(n)
                    main.post {
                        events?.success(
                            mapOf(
                                "type" to "data",
                                "bytes" to chunk,
                            ),
                        )
                    }
                }
            } catch (_: IOException) {
                // closed
            } finally {
                main.post {
                    events?.success(mapOf("type" to "closed"))
                }
            }
        }.also {
            it.name = "companion-spp-rx"
            it.isDaemon = true
            it.start()
        }
    }

    private fun closeLocked() {
        reader?.interrupt()
        reader = null
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
    }
}
