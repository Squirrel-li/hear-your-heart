package com.itap.remote_sensor_simulator

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.ParcelUuid
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.util.UUID

// ponytail: keep the BLE server in the foreground Activity; add a foreground service only if background advertising is required.
class MainActivity : FlutterActivity() {
    companion object {
        private const val METHOD_CHANNEL = "com.itap.remote_sensor_simulator/ble"
        private const val EVENT_CHANNEL = "com.itap.remote_sensor_simulator/events"
        private const val PERMISSION_REQUEST = 4107

        private val SERVICE_UUID = UUID.fromString("0000FFF0-0000-1000-8000-00805F9B34FB")
        private val NOTIFY_UUID = UUID.fromString("0000FFF1-0000-1000-8000-00805F9B34FB")
        private val WRITE_UUID = UUID.fromString("0000FFF2-0000-1000-8000-00805F9B34FB")
        private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
    }

    private var eventSink: EventChannel.EventSink? = null
    private var bluetoothManager: BluetoothManager? = null
    private var adapter: BluetoothAdapter? = null
    private var gattServer: BluetoothGattServer? = null
    private var advertiser: BluetoothLeAdvertiser? = null
    private val connectedDevices = mutableMapOf<String, BluetoothDevice>()
    private val subscribedDevices = mutableMapOf<String, BluetoothDevice>()

    private var broadcastName = "BLE_sensor_ITAP"
    private var advertising = false
    private var lastNotification = ByteArray(0)

    private val gattCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connectedDevices[device.address] = device
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connectedDevices.remove(device.address)
                subscribedDevices.remove(device.address)
            }
            emitState()
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice,
            requestId: Int,
            offset: Int,
            characteristic: BluetoothGattCharacteristic,
        ) {
            val supported = characteristic.uuid == NOTIFY_UUID
            sendResponse(
                device,
                requestId,
                if (supported) BluetoothGatt.GATT_SUCCESS else BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED,
                offset,
                if (supported) lastNotification else null,
            )
        }

        override fun onDescriptorReadRequest(
            device: BluetoothDevice,
            requestId: Int,
            offset: Int,
            descriptor: BluetoothGattDescriptor,
        ) {
            if (descriptor.uuid == CCCD_UUID) {
                val value = if (subscribedDevices.containsKey(device.address)) {
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                } else {
                    BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
                }
                sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value)
            } else {
                sendResponse(device, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, offset, null)
            }
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice,
            requestId: Int,
            descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean,
            responseNeeded: Boolean,
            offset: Int,
            value: ByteArray?,
        ) {
            if (descriptor.uuid == CCCD_UUID && descriptor.characteristic.uuid == NOTIFY_UUID) {
                val enabled = value?.contentEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == true
                if (enabled) {
                    subscribedDevices[device.address] = device
                } else {
                    subscribedDevices.remove(device.address)
                }
                if (responseNeeded) {
                    sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
                }
                emitState()
                return
            }
            if (responseNeeded) {
                sendResponse(device, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, offset, null)
            }
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice,
            requestId: Int,
            characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean,
            responseNeeded: Boolean,
            offset: Int,
            value: ByteArray?,
        ) {
            if (characteristic.uuid == WRITE_UUID) {
                val hex = value?.joinToString(" ") { byte ->
                    "%02X".format(byte.toInt() and 0xff)
                } ?: ""
                val event = stateMap("fff2_write").toMutableMap()
                event["hex"] = hex
                emit(event)
                if (responseNeeded) {
                    sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
                }
            } else if (responseNeeded) {
                sendResponse(device, requestId, BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED, offset, null)
            }
        }
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
            advertising = true
            emitState()
        }

        override fun onStartFailure(errorCode: Int) {
            advertising = false
            emitState(error = "advertise_failed")
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, METHOD_CHANNEL)
            .setMethodCallHandler(::onMethodCall)

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, EVENT_CHANNEL)
            .setStreamHandler(object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    eventSink = events
                    emitState()
                }

                override fun onCancel(arguments: Any?) {
                    eventSink = null
                }
            })
    }

    private fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "initialize" -> result.success(initializeBle())
            "setName" -> result.success(setName(call.argument<String>("name")))
            "startAdvertising" -> result.success(startAdvertising())
            "stopAdvertising" -> result.success(stopAdvertising())
            "notify" -> result.success(notify(call.argument<ByteArray>("bytes")))
            else -> result.notImplemented()
        }
    }

    private fun initializeBle(): Map<String, Any?> {
        if (!packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) {
            return stateMap(error = "ble_unsupported")
        }
        bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        adapter = bluetoothManager?.adapter
        if (adapter == null) return stateMap(error = "ble_unsupported")

        if (!hasBluetoothPermissions()) {
            requestPermissions(requiredPermissions(), PERMISSION_REQUEST)
            return stateMap(error = "permission_required")
        }
        if (adapter?.isEnabled == true) openGattServer()
        return stateMap(error = if (adapter?.isEnabled == true) null else "bluetooth_disabled")
    }

    private fun setName(value: String?): Map<String, Any?> {
        val name = value?.trim()
        if (name.isNullOrEmpty() || name.length > 20 || name.any { it == '\n' || it == '\r' }) {
            return stateMap(error = "invalid_name")
        }
        broadcastName = name
        try {
            if (hasBluetoothPermissions()) adapter?.name = broadcastName
        } catch (_: SecurityException) {
            return stateMap(error = "permission_required")
        }
        if (advertising) {
            stopAdvertisingInternal()
            startAdvertisingInternal()
        }
        return stateMap()
    }

    private fun startAdvertising(): Map<String, Any?> {
        val error = readyError()
        if (error != null) return stateMap(error = error)
        val startError = startAdvertisingInternal()
        return stateMap(error = startError)
    }

    private fun startAdvertisingInternal(): String? {
        val localAdapter = adapter ?: return "ble_unsupported"
        if (!localAdapter.isMultipleAdvertisementSupported) return "ble_unsupported"
        val localAdvertiser = try {
            localAdapter.bluetoothLeAdvertiser
        } catch (_: SecurityException) {
            return "permission_required"
        } ?: return "ble_unsupported"

        try {
            localAdapter.name = broadcastName
            val settings = AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setConnectable(true)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
                .build()
            val data = AdvertiseData.Builder()
                .setIncludeDeviceName(true)
                .build()
            val scanResponse = AdvertiseData.Builder()
                .setIncludeDeviceName(false)
                .addServiceUuid(ParcelUuid(SERVICE_UUID))
                .build()
            advertiser = localAdvertiser
            localAdvertiser.startAdvertising(settings, data, scanResponse, advertiseCallback)
            advertising = true
            emitState()
            return null
        } catch (_: SecurityException) {
            return "permission_required"
        } catch (_: IllegalArgumentException) {
            return "advertise_failed"
        }
    }

    private fun stopAdvertising(): Map<String, Any?> {
        stopAdvertisingInternal()
        return stateMap()
    }

    private fun stopAdvertisingInternal() {
        try {
            advertiser?.stopAdvertising(advertiseCallback)
        } catch (_: SecurityException) {
        }
        advertiser = null
        advertising = false
        emitState()
    }

    private fun notify(bytes: ByteArray?): Map<String, Any?> {
        if (bytes == null || bytes.size != 21) return resultMap(false, "invalid_payload")
        if (subscribedDevices.isEmpty()) return resultMap(false, "not_connected")

        val characteristic = gattServer
            ?.getService(SERVICE_UUID)
            ?.getCharacteristic(NOTIFY_UUID)
            ?: return resultMap(false, "gatt_server_failed")

        lastNotification = bytes.copyOf()
        var sent = true
        for (device in subscribedDevices.values.toList()) {
            try {
                val status = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    gattServer?.notifyCharacteristicChanged(device, characteristic, false, bytes)
                        ?: BluetoothGatt.GATT_FAILURE
                } else {
                    characteristic.value = bytes
                    if (gattServer?.notifyCharacteristicChanged(device, characteristic, false) == true) {
                        BluetoothGatt.GATT_SUCCESS
                    } else {
                        BluetoothGatt.GATT_FAILURE
                    }
                }
                if (status != BluetoothGatt.GATT_SUCCESS) sent = false
            } catch (_: SecurityException) {
                sent = false
            }
        }
        return resultMap(sent, if (sent) null else "notify_failed")
    }

    private fun readyError(): String? {
        if (!packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) return "ble_unsupported"
        if (!hasBluetoothPermissions()) return "permission_required"
        val localAdapter = adapter ?: return "ble_unsupported"
        if (!localAdapter.isEnabled) return "bluetooth_disabled"
        openGattServer()
        return if (gattServer == null) "gatt_server_failed" else null
    }

    private fun openGattServer() {
        if (gattServer != null) return
        val manager = bluetoothManager ?: return
        try {
            val server = manager.openGattServer(this, gattCallback) ?: return
            val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)
            val notify = BluetoothGattCharacteristic(
                NOTIFY_UUID,
                BluetoothGattCharacteristic.PROPERTY_NOTIFY or BluetoothGattCharacteristic.PROPERTY_READ,
                BluetoothGattCharacteristic.PERMISSION_READ,
            )
            notify.addDescriptor(
                BluetoothGattDescriptor(
                    CCCD_UUID,
                    BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE,
                ),
            )
            val write = BluetoothGattCharacteristic(
                WRITE_UUID,
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattCharacteristic.PERMISSION_WRITE,
            )
            service.addCharacteristic(notify)
            service.addCharacteristic(write)
            if (server.addService(service)) {
                gattServer = server
            } else {
                server.close()
            }
        } catch (_: SecurityException) {
            gattServer = null
        }
    }

    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            emptyArray()
        }

    private fun hasBluetoothPermissions(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            requiredPermissions().all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun sendResponse(
        device: BluetoothDevice,
        requestId: Int,
        status: Int,
        offset: Int,
        value: ByteArray?,
    ) {
        try {
            gattServer?.sendResponse(device, requestId, status, offset, value)
        } catch (_: SecurityException) {
        }
    }

    private fun emitState(error: String? = null) {
        emit(stateMap(error = error))
    }

    private fun emit(event: Map<String, Any?>) {
        runOnUiThread { eventSink?.success(event) }
    }

    private fun stateMap(type: String = "state", error: String? = null): HashMap<String, Any?> =
        hashMapOf(
            "type" to type,
            "bluetoothAvailable" to (adapter != null && packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)),
            "bluetoothEnabled" to (adapter?.isEnabled == true),
            "advertising" to advertising,
            "connected" to connectedDevices.isNotEmpty(),
            "subscribed" to subscribedDevices.isNotEmpty(),
            "name" to broadcastName,
            "error" to error,
        )

    private fun resultMap(ok: Boolean, error: String?): HashMap<String, Any?> =
        hashMapOf("ok" to ok, "error" to error)

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != PERMISSION_REQUEST) return
        val granted = grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }
        if (granted && adapter?.isEnabled == true) openGattServer()
        emitState(error = if (granted) null else "permission_required")
    }

    override fun onDestroy() {
        stopAdvertisingInternal()
        gattServer?.close()
        gattServer = null
        connectedDevices.clear()
        subscribedDevices.clear()
        super.onDestroy()
    }
}






